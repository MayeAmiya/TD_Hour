#pragma once

#include "core/container/container_types.h"
#include "core/math/fixed/q32_32.h"

#include "TerrainMap.h"
#include "game/player/PlayerTypes.h"
#include <cstddef>
#include <cstdint>
#include <optional>
namespace game::terrain {

// RefCode drives shroud from PartitionManager's authored 40-world-unit grid,
// not from the 10-unit heightfield tessellation. Keeping this independent of
// terrain cell size also prevents terrain LOD/topology work from changing
// gameplay sight semantics.
inline constexpr math::q32_32 kMapVisibilityCellWorldSize{40};
inline constexpr uint32_t kDefaultUnlookPersistenceMilliseconds = 5000u;
inline constexpr uint32_t kDefaultFogTransitionMilliseconds = 1000u;

// This is the three-state result exposed by RefCode's PartitionCell: a cell
// is either actively clear, passively explored/fogged, or fully shrouded.
// The authority retains the underlying signed look/shroud counters so script
// pulses preserve the old add/remove transition rules rather than merely
// painting a modern boolean mask.
enum class MapVisibilityCellState : uint8_t {
    Shrouded = 0,
    Fogged = 1,
    Clear = 2,
};

struct MapVisibilityPlayerSnapshot final {
    engine::PlayerId player = engine::INVALID_PLAYER_ID;
    uint64_t revision = 0;
    container::SharedPtr<const container::Vector<MapVisibilityCellState>> cells;
    // Presentation-only 0..255 luminance which converges to the discrete
    // authority state over the session-frozen transition duration.
    container::SharedPtr<const container::Vector<uint8_t>> visualLevels;
};

struct MapVisibilityDirtyRegion final {
    int32_t minX = 0;
    int32_t minY = 0;
    int32_t maxX = -1;
    int32_t maxY = -1;

    [[nodiscard]] bool isValid() const noexcept {
        return minX >= 0 && minY >= 0 && maxX >= minX && maxY >= minY;
    }
    void include(int32_t x, int32_t y) noexcept;
    void includeAll(int32_t width, int32_t height) noexcept;
};

// Value-only sight contribution submitted by the confirmed simulation. The
// authority deliberately receives no ECS entity, ObjectId, template pointer,
// or renderer resource; GameSession resolves those mutable owners before this
// immutable boundary.
struct MapVisibilityDynamicLooker final {
    // Stable, caller-owned identity for delayed unlook. The same object may
    // publish more than one channel (allied clearing and reveal-to-all), so
    // the caller must include the channel and target player in this value.
    // Zero remains a probe/script convenience with no retained history.
    uint64_t identity = 0;
    engine::PlayerId player = engine::INVALID_PLAYER_ID;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 radius{};
};

// Per-target-player active shroud contribution. GameSession expands one
// object-centric hostile shrouder into these detached player records after
// applying diplomacy; the terrain authority remains unaware of alliances.
struct MapVisibilityDynamicShrouder final {
    engine::PlayerId player = engine::INVALID_PLAYER_ID;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 radius{};
};

// Immutable presentation/read snapshot.  It owns only values, so renderer/UI
// code never reaches into TerrainLogic or mutable script state after a frame
// has been sealed. `width`/`height` describe terrain *cells* (not height
// samples), with world origin derived from `borderSize` and cellWorldSize.
struct MapVisibilitySnapshot final {
    uint64_t revision = 0;
    uint64_t terrainLayoutRevision = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t borderSize = 0;
    float originX = 0.0f;
    float originY = 0.0f;
    // Presentation-only projections. The raw fields below are the canonical
    // detached visibility layout consumed by simulation and raster queries.
    float cellWorldSize = 40.0f;
    int64_t originXRaw = 0;
    int64_t originYRaw = 0;
    int64_t cellWorldSizeRaw = 0;
    bool renderingActive = false;
    MapVisibilityDirtyRegion dirtyRegion;
    container::Vector<MapVisibilityPlayerSnapshot> players;

    [[nodiscard]] const MapVisibilityPlayerSnapshot* player(
        engine::PlayerId id) const noexcept;
    [[nodiscard]] MapVisibilityCellState cellState(engine::PlayerId player,
                                                    int32_t x,
                                                    int32_t y) const noexcept;
    // Object::getShroudedStatus treats both CLEAR and PARTIAL_CLEAR as
    // discovered. Test every visibility cell intersecting the object's
    // detached bounding-circle footprint so large structures do not become
    // invisible merely because their center cell remains shrouded.
    [[nodiscard]] bool footprintHasClearCellRaw(
        engine::PlayerId player, int64_t centerXRaw,
        int64_t centerYRaw, int64_t radiusRaw) const noexcept;
};

// Session-owned replacement for PartitionManager shroud state. It deliberately
// does not know about ECS objects, alliances, or renderer resources. Confirmed
// simulation submits only detached dynamic looker values; script reveal/shroud
// operations and durable explored state remain authoritative in the same
// per-player grid.
class MapVisibilityAuthority final {
public:
    bool initialize(const TerrainMap& terrain,
                    container::Span<const engine::PlayerId> players);
    void clear() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept {
        return m_width > 0 && m_height > 0;
    }
    [[nodiscard]] uint64_t revision() const noexcept { return m_revision; }

    // Rebuilds every player's temporary sight count from this complete frame
    // contribution list. Cells seen at least once remain explored after their
    // final dynamic looker leaves. Counter churn which does not change the
    // public Shrouded/Fogged/Clear result publishes no new revision.
    [[nodiscard]] bool updateDynamicLookers(
        container::Span<const MapVisibilityDynamicLooker> lookers,
        container::Span<const MapVisibilityDynamicShrouder> shrouders = {},
        uint64_t confirmedTick = 0,
        uint32_t unlookPersistenceTicks = 0,
        uint32_t fogTransitionTicks = 0);

    // Multiplayer UseShroud and observer policy are frozen by GameSession.
    // Disabling the policy projects every player cell as Clear without
    // destroying the underlying counters, so replay/session restore can
    // re-enable the exact authoritative state deterministically.
    [[nodiscard]] bool setShroudEnabled(bool enabled) noexcept;
    [[nodiscard]] bool shroudEnabled() const noexcept { return m_shroudEnabled; }

    // RefCode's REFRESH_RADAR rebuilds its terrain image without changing
    // shroud counters. Publish a fresh immutable revision so radar clients
    // can invalidate their cache even when no cell changed.
    [[nodiscard]] bool refresh() noexcept;

    // MAP_REVEAL_AT_WAYPOINT and MAP_SHROUD_AT_WAYPOINT use an immediate
    // add/remove pair. It looks redundant, but the legacy cell counter turns
    // an initially shrouded cell into fogged (or vice versa) as a result.
    [[nodiscard]] bool revealCircle(engine::PlayerId player,
                                    math::q32_32 centerX,
                                    math::q32_32 centerY,
                                    math::q32_32 radius) noexcept;
    [[nodiscard]] bool shroudCircle(engine::PlayerId player,
                                    math::q32_32 centerX,
                                    math::q32_32 centerY,
                                    math::q32_32 radius) noexcept;
    [[nodiscard]] bool revealAll(engine::PlayerId player) noexcept;
    [[nodiscard]] bool shroudAll(engine::PlayerId player) noexcept;
    [[nodiscard]] bool revealAllPermanently(engine::PlayerId player) noexcept;
    [[nodiscard]] bool undoRevealAllPermanently(engine::PlayerId player) noexcept;

    // A named permanent reveal owns one durable active look.  RefCode treats
    // a duplicate reveal name as an authoring error; retain the first record
    // instead of silently layering an unremovable second reveal.  Callers
    // supply a resolved PlayerId because named reveals never use the old
    // empty-side = every-human-player fallback.
    [[nodiscard]] bool createNamedPermanentReveal(container::String name,
                                                   engine::PlayerId player,
                                                   math::q32_32 centerX,
                                                   math::q32_32 centerY,
                                                   math::q32_32 radius);
    [[nodiscard]] bool undoNamedPermanentReveal(container::StringView name) noexcept;

    [[nodiscard]] container::SharedPtr<const MapVisibilitySnapshot> snapshot() const noexcept {
        return m_snapshot;
    }

private:
    struct Cell final {
        int16_t currentShroud = 1;
        int16_t activeShroud = 0;
        uint16_t dynamicLookerCount = 0;
        uint16_t dynamicShrouderCount = 0;
        bool explored = false;
        uint8_t visualLevel = 0;
    };
    struct PlayerGrid final {
        engine::PlayerId player = engine::INVALID_PLAYER_ID;
        container::Vector<Cell> cells;
        container::Vector<uint16_t> nextDynamicLookerCounts;
        container::Vector<uint16_t> nextDynamicShrouderCounts;
        container::SharedPtr<const container::Vector<MapVisibilityCellState>> snapshotCells;
        container::SharedPtr<const container::Vector<uint8_t>> snapshotVisualLevels;
        uint64_t snapshotRevision = 0;
        bool snapshotDirty = true;
    };
    struct NamedPermanentReveal final {
        container::String name;
        engine::PlayerId player = engine::INVALID_PLAYER_ID;
        math::q32_32 centerX{};
        math::q32_32 centerY{};
        math::q32_32 radius{};
    };
    struct ActiveDynamicLooker final {
        MapVisibilityDynamicLooker value;
        uint64_t seenGeneration = 0;
    };
    struct PendingDynamicUnlook final {
        MapVisibilityDynamicLooker value;
        uint64_t expiresAfterTick = 0;
    };
    enum class CircleOperation : uint8_t {
        RevealPulse,
        ShroudPulse,
        AddLooker,
        RemoveLooker,
    };

    [[nodiscard]] PlayerGrid* grid(engine::PlayerId player) noexcept;
    [[nodiscard]] const PlayerGrid* grid(engine::PlayerId player) const noexcept;
    [[nodiscard]] size_t index(int32_t x, int32_t y) const noexcept;
    [[nodiscard]] bool applyCircle(PlayerGrid& grid,
                                   math::q32_32 centerX,
                                   math::q32_32 centerY,
                                   math::q32_32 radius,
                                   bool reveal) noexcept;
    [[nodiscard]] bool applyAll(PlayerGrid& grid, bool reveal, bool permanent) noexcept;
    [[nodiscard]] bool applyPermanentCircle(PlayerGrid& grid,
                                            math::q32_32 centerX,
                                            math::q32_32 centerY,
                                            math::q32_32 radius,
                                            bool add) noexcept;
    [[nodiscard]] bool mutateCircle(PlayerGrid& grid,
                                    math::q32_32 centerX,
                                    math::q32_32 centerY,
                                    math::q32_32 radius,
                                    CircleOperation operation) noexcept;
    void accumulateDynamicLooker(PlayerGrid& grid,
                                 math::q32_32 centerX,
                                 math::q32_32 centerY,
                                 math::q32_32 radius) noexcept;
    void accumulateDynamicShrouder(PlayerGrid& grid,
                                   math::q32_32 centerX,
                                   math::q32_32 centerY,
                                   math::q32_32 radius) noexcept;
    [[nodiscard]] bool sameLookFootprint(
        const MapVisibilityDynamicLooker& left,
        const MapVisibilityDynamicLooker& right) const noexcept;
    void queueDynamicUnlook(const MapVisibilityDynamicLooker& looker,
                            uint64_t confirmedTick,
                            uint32_t persistenceTicks);
    [[nodiscard]] bool advanceVisualLevels(uint64_t confirmedTick,
                                           uint32_t transitionTicks) noexcept;
    static void addLooker(Cell& cell) noexcept;
    static void removeLooker(Cell& cell) noexcept;
    static void addShrouder(Cell& cell) noexcept;
    static void removeShrouder(Cell& cell) noexcept;
    static MapVisibilityCellState stateFor(Cell cell) noexcept;
    [[nodiscard]] bool activateRendering() noexcept;
    void markFullDirty() noexcept;
    void markChanged() noexcept;
    void rebuildSnapshot();

    int32_t m_width = 0;
    int32_t m_height = 0;
    int32_t m_borderSize = 0;
    math::q32_32 m_originX{};
    math::q32_32 m_originY{};
    math::q32_32 m_cellWorldSize = kMapVisibilityCellWorldSize;
    uint64_t m_terrainLayoutRevision = 0;
    uint64_t m_revision = 0;
    bool m_renderingActive = false;
    bool m_shroudEnabled = true;
    uint64_t m_dynamicLookerGeneration = 0;
    uint64_t m_lastVisualTransitionTick = UINT64_MAX;
    MapVisibilityDirtyRegion m_pendingDirty;
    container::Array<std::optional<size_t>, engine::PLAYER_REGISTRY_CAPACITY> m_gridByPlayer{};
    container::Vector<PlayerGrid> m_grids;
    container::Vector<NamedPermanentReveal> m_namedPermanentReveals;
    container::HashMap<uint64_t, ActiveDynamicLooker> m_activeDynamicLookers;
    container::Vector<PendingDynamicUnlook> m_pendingDynamicUnlooks;
    container::SharedPtr<const MapVisibilitySnapshot> m_snapshot;
};

} // namespace game::terrain
