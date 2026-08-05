#pragma once

#include "core/container/hash_containers.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/base/ObjectVeterancy.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>
namespace game {
struct ThingTemplate;
}

namespace engine {

// Fixed-only footprint override used by runtime structural replacement.
// Authoring Geometry remains float in ThingTemplate, but once a live object's
// footprint crosses a simulation transaction it must not be reconstructed
// from the presentation fields of ObjectGeometryComponent.
struct ObjectSpawnGeometryOverride final {
    ObjectGeometryShape shape = ObjectGeometryShape::Sphere;
    bool isSmall = true;
    math::q32_32 majorRadius{int32_t{1}};
    math::q32_32 minorRadius{int32_t{1}};
    math::q32_32 height{int32_t{1}};
    math::q32_32 boundingCircleRadius{int32_t{1}};
    math::q32_32 boundingSphereRadius{int32_t{1}};
};

struct ObjectTerrainFlattenPlacement final {
    // Pose used by the legacy LIKE_EXISTING stage before later dispositions
    // are allowed to move the object elsewhere.
    ObjectFixedTransformComponent footprintTransform{
        .authoritative = true};
    math::q32_32 groundSampleX{};
    math::q32_32 groundSampleY{};
    // ON_GROUND_ALIGNED/SEND_IT_* overwrite the intermediate snap later in
    // RefCode, so those combinations flatten without rewriting final Z.
    bool adjustFinalObjectZ = true;
};

// Typed projection of WorldBuilder's per-object property panel. These values
// are deliberately separate from generic spawn overrides: ZH applies them
// after module initialization/onCreate and before onBuildComplete, while OCL
// and production health overrides retain their existing construction-time
// semantics.
struct MapObjectInstanceOverrides final {
    std::optional<math::q32_32> maximumHealth;
    std::optional<math::q32_32> initialHealthFraction;
    std::optional<game::ObjectVeterancyLevel> veterancy;
    container::Vector<container::String> grantedUpgrades;
    std::optional<bool> enabled;
    std::optional<bool> powered;
    std::optional<bool> indestructible;
    std::optional<bool> unsellable;
    std::optional<bool> selectable;
    std::optional<bool> aiRecruitable;
    std::optional<bool> playerTargetable;
    std::optional<ObjectAIAttitude> aggressiveness;
    std::optional<math::q32_32> stoppingDistance;
    std::optional<math::q32_32> visionRange;
    std::optional<math::q32_32> shroudClearingRange;
    std::optional<bool> night;
    std::optional<bool> snow;
    std::optional<container::String> ambientSound;
    std::optional<bool> ambientSoundEnabled;
    std::optional<bool> ambientSoundLooping;
    std::optional<int32_t> ambientSoundLoopCount;
    std::optional<float> ambientSoundMinVolume;
    std::optional<float> ambientSoundVolume;
    std::optional<float> ambientSoundMinRange;
    std::optional<float> ambientSoundMaxRange;
    std::optional<uint8_t> ambientSoundPriority;
};

// Value-only request consumed by the one authoritative creation path. It is
// intentionally usable by map import, production, scripts and player orders
// without exposing an EnTT entity or renderer resource to any of them.
struct ObjectSpawnRequest final {
    container::String templateName;
    PlayerId owner = INVALID_PLAYER_ID;
    // GameSession resolves an omitted value to the owning player's default
    // ObjectTeam before this request reaches ObjectLifecycle.  Keeping the
    // complete assignment on the creation request lets map import and future
    // production publish no half-owned entities.
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    // Required canonical pose. Map/INI/script authoring adapters quantize
    // before constructing this request; runtime producers already own this
    // representation. ObjectLifecycle creates TransformComponent only as the
    // one-way presentation projection after the transaction is admitted.
    ObjectFixedTransformComponent transform{.authoritative = true};
    // Default/SpawnPoint production exits and legacy layer-aware creation
    // may freeze an explicit source layer.  Omitted map/script creation lets
    // ObjectSimulation select the closest terrain surface from authored Z.
    std::optional<uint32_t> initialPathfindLayer;
    ObjectCreationOrigin origin = ObjectCreationOrigin::System;
    uint64_t confirmedTick = 0;
    // Map object data remains optional and detached. A later factory/module
    // may interpret more properties, but the lifecycle always records the
    // whole authored record atomically with object creation.
    std::optional<MapObjectProvenanceComponent> mapProvenance;
    std::optional<MapObjectInstanceOverrides> mapInstanceOverrides;
    // Map Object properties have distinct legacy meanings: objectMaxHP is an
    // absolute max-health override, while objectInitialHealth is a fraction
    // applied after that override. Both are quantized at authoring ingress.
    std::optional<math::q32_32> maximumHealthOverride;
    std::optional<math::q32_32> initialHealthFraction;
    // Rebuild holes inherit the dead structure's gameplay footprint in the
    // same unpublished transaction; render bounds remain template-owned.
    std::optional<ObjectSpawnGeometryOverride> geometryOverride;
    // Canonical ObjectStatus mask applied before the creation leaves the
    // lifecycle transaction. Callers use this for typed initial facts such
    // as RECONSTRUCTING; unknown bits are rejected by ObjectStatusSystem.
    uint64_t initialStatusMask = 0;
    // Factory production supplies this immutable value edge at the same
    // transaction boundary as ObjectId/Owner/Team creation.  It is absent for
    // map, script and projectile creation and never stores an ECS entity.
    std::optional<ObjectProducedByComponent> producedBy;
    ObjectId producer = INVALID_OBJECT_ID;
    // Dozer/build-placement ingress supplies the stable ObjectId that the
    // original StructureBody stored as m_constructorObjectID.  ObjectLifecycle
    // materializes it only for StructureBody-derived bodies, independently of
    // ObjectProducedByComponent, and never retains the constructor's entity.
    ObjectId constructedBy = INVALID_OBJECT_ID;
    // Rebuild/morph creation moves every live script alias from this source
    // before lifecycle events are published. This must be part of the spawn
    // transaction: a post-return transfer is too late once DestroyRequested
    // has marked the old aliases destroyed.
    ObjectId inheritScriptNamesFrom = INVALID_OBJECT_ID;
    // Map-authored objectName / future script-created name. ObjectLifecycle
    // transports this value; GameSession binds it to ScriptObjectIndex once
    // creation crosses the session boundary.
    container::String scriptName;
    // ScriptActions::doCreateObject permits a historical name only when its
    // prior Object is effectively dead, then transfers that name to the new
    // Object. Keep this opt-in at the authoritative creation request so a
    // failed name bind cannot leave a script-created unbound entity behind.
    // Ordinary map/production callers preserve their existing bind-only
    // behavior unless they deliberately request this legacy replacement mode.
    bool replaceEffectivelyDeadScriptName = false;
    // Explicit completion accounting.  Creation origin is intentionally not
    // overloaded for this: a System spawn may be a scoreable SpawnBehavior
    // child, a ReplaceObject-built structure, or an unscored projectile/OCL
    // helper.  Callers freeze the exact legacy callback semantics here.
    bool scoreAsBuilt = false;
    bool academyAsProduction = false;
    bool scoreConstructionCost = false;
    // Future dozer/build-placement ingress can publish an object at
    // construction start while deferring game-side CreateModule completion.
    // Existing map/script/system/production spawns remain completed objects.
    bool startsUnderConstruction = false;
    // RefCode flattens normal dozer/worker construction sites and the
    // structure branch of OCL LIKE_EXISTING before adding their footprint to
    // pathfinding. The central spawn transaction performs it only after all
    // abortable owner/team/name invariants have committed.
    bool flattenTerrainForStructure = false;
    std::optional<ObjectTerrainFlattenPlacement> terrainFlattenPlacement;
};

enum class ObjectDestroyReason : uint8_t {
    Script,
    Combat,
    PlayerSell,
    SessionShutdown,
    System,
};

enum class ObjectLifecycleEventKind : uint8_t {
    Created,
    DestroyRequested,
    Destroyed,
    OwnershipChanged,
};

// The event contains no ECS entity. Consumers such as ScriptObjectIndex,
// selection, spatial indexing and render extraction must resolve state through
// ObjectId at their own phase boundary instead of holding a dangling pointer.
struct ObjectLifecycleEvent final {
    ObjectLifecycleEventKind kind = ObjectLifecycleEventKind::Created;
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId previousOwner = INVALID_PLAYER_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectCreationOrigin origin = ObjectCreationOrigin::System;
    ObjectDestroyReason destroyReason = ObjectDestroyReason::System;
    uint64_t confirmedTick = 0;
    container::String templateName;
};

struct ObjectSpawnResult final {
    ObjectId object = INVALID_OBJECT_ID;
    std::optional<ecs::entity> entity;

    [[nodiscard]] explicit operator bool() const noexcept {
        return object && entity.has_value();
    }
};

// Session-owned modern replacement for the original ThingFactory + GameLogic
// object-vector structural path. It has exactly one ObjectId allocator and
// ObjectId->entity table, batches destroy structural changes, and publishes a
// stable event stream. It owns neither content stores nor game modules.
class ObjectLifecycle final {
public:
    explicit ObjectLifecycle(ecs::registry& registry) noexcept;

    ObjectLifecycle(const ObjectLifecycle&) = delete;
    ObjectLifecycle& operator=(const ObjectLifecycle&) = delete;

    void reset(bool clearRegistry = true) noexcept;

    [[nodiscard]] ObjectSpawnResult create(const ObjectSpawnRequest& request,
                                           const game::ThingTemplate& thingTemplate,
                                           const ObjectSimulationRules& rules = {});
    [[nodiscard]] bool requestDestroy(ObjectId object, ObjectDestroyReason reason,
                                      uint64_t confirmedTick);
    // Applies requested destroys in request order. Call only at an explicit
    // simulation phase boundary; logical unavailability is published at
    // request time, so script actions never need a global structural flush.
    [[nodiscard]] size_t flushRequestedDestroys();
    [[nodiscard]] bool changeOwner(ObjectId object, PlayerId newOwner,
                                   uint64_t confirmedTick,
                                   bool allowPendingDestroy = false);

    // Captures the current private lifecycle-event tail before an operation
    // that may need to abandon a newly created entity.  The corresponding
    // abort operation is intentionally narrow: it never consumes already
    // queued lifecycle events or flushes other pending destroys.
    [[nodiscard]] size_t eventCheckpoint() const noexcept { return m_events.size(); }
    // Destroys exactly one still-unpublished, Alive entity created after
    // `eventCheckpoint`.  This is for GameSession's creation transaction
    // rollback only; it deliberately does not recycle the ObjectId, publish
    // DestroyRequested/Destroyed events, or touch another object's pending
    // destroy request.  Returns false if the requested entity cannot be
    // proved to be an unpublished creation in this event suffix.
    [[nodiscard]] bool abortUnpublishedCreate(ObjectId object, size_t eventCheckpoint);

    [[nodiscard]] std::optional<ecs::entity> entityFromId(ObjectId object) const noexcept;
    // Narrow deferred-transaction lookup. A PendingDestroy entity is still
    // structurally present until flushRequestedDestroys(); lethal callbacks
    // that were already queued in the same confirmed batch may inspect its
    // value state, but ordinary gameplay systems must use entityFromId().
    [[nodiscard]] std::optional<ecs::entity>
    entityFromIdIncludingPending(ObjectId object) const noexcept;
    [[nodiscard]] ObjectId objectIdFromEntity(ecs::entity entity) const noexcept;
    [[nodiscard]] bool isPendingDestroy(ObjectId object) const noexcept;
    [[nodiscard]] size_t objectCount() const noexcept { return m_entitiesById.size(); }
    [[nodiscard]] bool exhausted() const noexcept { return m_ids.exhausted(); }

    [[nodiscard]] container::Vector<ObjectLifecycleEvent> takeEvents();

private:
    struct PendingDestroy final {
        ObjectId object = INVALID_OBJECT_ID;
        ObjectDestroyReason reason = ObjectDestroyReason::System;
        uint64_t confirmedTick = 0;
    };

    ecs::registry& m_registry;
    ObjectIdAllocator m_ids;
    container::HashMap<ObjectId, ecs::entity> m_entitiesById;
    container::Vector<PendingDestroy> m_pendingDestroys;
    container::Vector<ObjectLifecycleEvent> m_events;
};

} // namespace engine
