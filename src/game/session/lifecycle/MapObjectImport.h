#pragma once

#include "game/terrain/MapHeightfieldLoader.h"

#include <cstddef>

namespace engine {

class GameSession;

// Observable result of the map-object import phase.  A map can contain
// waypoints, lights, editor markers, bridges and obsolete template names, so
// a successful map load must not imply that every serialized record became a
// gameplay object.
struct MapObjectSpawnReport {
    size_t sourceRecordCount = 0;
    size_t spawnedCount = 0;
    size_t clientTerrainObjectCount = 0;
    size_t disabledClientDecorationCount = 0;
    size_t clientTerrainVisualFailureCount = 0;
    // Terrain bridge point pairs remain metadata rather than ordinary map
    // Things, but each valid pair creates one synthetic GenericBridge Body
    // authority. Track that extra object separately so source-record
    // spawned/skipped accounting stays a partition of the CkMp records.
    size_t terrainBridgeAuthorityCount = 0;
    size_t terrainBridgeAuthorityFailureCount = 0;
    size_t skippedWaypointCount = 0;
    size_t skippedRoadOrBridgeCount = 0;
    size_t skippedClientMetadataCount = 0;
    size_t skippedEmptyNameCount = 0;
    size_t skippedUnknownTemplateCount = 0;
    size_t skippedInvalidTransformCount = 0;
    size_t creationFailureCount = 0;
    size_t neutralOwnerFallbackCount = 0;
    // `originalOwner` may name a CkMp Scenario Team rather than a Player.
    // Import now passes its resolved live primary ObjectTeam atomically in
    // ObjectSpawnRequest, rather than spawning first and attaching a second
    // script-only membership afterward.
    size_t scenarioTeamMemberCount = 0;
    size_t scenarioTeamResolutionFailureCount = 0;
    // `objectName` is the map-authored script identity, distinct from the
    // Thing template name.  Retain successful bindings for ScriptRuntime and
    // make malformed duplicate live names observable instead of silently
    // retargeting a scenario trigger.
    size_t scriptNameBoundCount = 0;
    size_t scriptNameConflictCount = 0;

    [[nodiscard]] size_t skippedCount() const noexcept {
        return skippedWaypointCount + skippedRoadOrBridgeCount +
               skippedClientMetadataCount + skippedEmptyNameCount +
               skippedUnknownTemplateCount + skippedInvalidTransformCount +
               clientTerrainVisualFailureCount + creationFailureCount +
               disabledClientDecorationCount;
    }
};

// Converts detached CkMp records into normal game entities.  This is a
// game-domain startup step; it never grants terrain or rendering code access
// to the session registry.
class MapObjectImport final {
public:
    [[nodiscard]] static MapObjectSpawnReport import(
        GameSession& session,
        const game::terrain::TerrainHeightfieldData& source);
};

} // namespace engine
