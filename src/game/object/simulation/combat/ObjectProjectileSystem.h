#pragma once

#include "core/container/container_types.h"
#include "core/container/hash_containers.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/simulation/combat/ObjectHistoricWeaponTypes.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/plan/combat/ObjectProjectilePlanTypes.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <optional>
namespace game {
struct ObjectArchetype;
struct ThingTemplate;
namespace terrain {
class TerrainLogic;
}
} // namespace game

namespace engine {

class GameContentSnapshot;
class ObjectSpatialIndex;
class PlayerRegistry;

// Pure value emitted by CombatSystem at the same confirmed tick as a shot.
// GameSession is the sole structural owner that turns it into an ECS Object.
struct ObjectProjectileSpawnRequest final {
    ObjectId launcher = INVALID_OBJECT_ID;
    uint32_t sourcePathfindLayer = 0;
    ObjectId intendedTarget = INVALID_OBJECT_ID;
    game::WeaponContentId detonationWeapon;
    game::WeaponBonusConditionMask launcherWeaponBonusConditions = 0;
    container::String projectileTemplate;
    // Commands/weapon selection convert their float ingress exactly once.
    // The request is then an ordinary fixed-point simulation value until a
    // presentation event is emitted.
    LogicFixedVec3 launchPosition{};
    // ProjectileStreamUpdate::setPosition receives Object::getPosition(),
    // not the weapon launch bone. Freeze that source anchor independently so
    // renderer shroud remains stable after the launcher moves or disappears.
    LogicFixedVec3 projectileStreamOwnerAnchorPosition{};
    LogicFixedQuaternion launchOrientation{};
    LogicFixedVec3 targetPosition{};
    // WeaponTemplate scatter converts an object attack into a position shot.
    // X/Y are sampled at fire admission; ProjectileSystem owns the terrain
    // projection because it already has the authoritative layered height map.
    uint32_t scatteredTargetPathfindLayer = 0;
    // Weapon aim points may use geometry centre (Dumb projectile behavior),
    // while MissileAI TryToFollowTarget tracks Object::getPosition(). Preserve
    // both values so the typed recipe can select the authored contract.
    LogicFixedVec3 intendedTargetBasePosition{};
    LogicFixedVec3 launcherVelocityUnitsPerSecond{};
    container::String projectileExhaust;
    // Generation of the legacy Weapon instance which owns ProjectileStream.
    // WeaponSet changes recreate those instances even if template/slot match.
    uint32_t projectileStreamOwnerGeneration = 0;
    // Frozen Weapon instance lane. RefCode owns one ProjectileStreamUpdate
    // per Weapon instance; preserving the selected slot keeps two equal
    // WeaponTemplates on the same launcher from sharing target-change state.
    uint8_t launchSlot = 0;
    uint32_t sourceShotSequence = 0;
    uint32_t sourceBarrelSequenceOrdinal = 0;
    // ScriptActions::NAMED_FIRE_WEAPON_FOLLOWING_WAYPOINT_PATH freezes the
    // resolved start node and immutable graph revision on the fire command.
    // Only a MissileAI projectile consumes this handoff.
    uint32_t waypointPathStartId = UINT32_MAX;
    uint64_t waypointGraphRevision = 0;
    bool hasLaunchOrientation = false;
    bool hasIntendedTargetBasePosition = false;
    bool targetWasScattered = false;
    // The firing system samples these exactly once from the session-owned
    // deterministic stream if the final projectile recipe has both
    // DumbProjectileBehavior(TumbleRandomly) and PhysicsBehavior. Keeping
    // the sampled values on the spawn request makes structural creation
    // independent of GameSession implementation details.
    bool hasTumbleAngularRates = false;
    ObjectPhysicsComponent::Scalar tumbleYawRate{};
    ObjectPhysicsComponent::Scalar tumblePitchRate{};
    ObjectPhysicsComponent::Scalar tumbleRollRate{};
    uint64_t confirmedTick = 0;
};

enum class ObjectProjectileEventKind : uint8_t {
    Spawned,
    Collided,
    ReachedDestination,
    Expired,
    // The line has no valid terrain cell, or a following target made the
    // recalculated Bezier path invalid. It is distinct from unsupported
    // module families so diagnostics can identify authored map/path data.
    PathInvalid,
    GarrisonCleared,
    Effect,
    GroundDecalBegin,
    GroundDecalEnd,
    UnsupportedTemplate,
};

// Presentation/diagnostic facts. Damage remains a separate value-only
// ObjectDamageRequest stream and structural destruction remains deferred.
struct ObjectProjectileEvent final {
    ObjectProjectileEventKind kind = ObjectProjectileEventKind::Spawned;
    ObjectId projectile = INVALID_OBJECT_ID;
    ObjectId launcher = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    uint32_t sourcePathfindLayer = 0;
    ObjectId target = INVALID_OBJECT_ID;
    uint32_t sourceShotSequence = 0;
    game::WeaponContentId detonationWeapon;
    // Terminal projectile events freeze their complete gameplay source
    // context before requesting destruction. Presentation projects `position`
    // to float, while OCL execution consumes it directly.
    LogicFixedVec3 position{};
    LogicFixedVec3 sourceVelocity{};
    ObjectPhysicsComponent::Scalar orientationRadians{};
    ObjectPhysicsComponent::Scalar pitchRadians{};
    ObjectPhysicsComponent::Scalar rollRadians{};
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    game::WeaponFxPolicy weaponFxPolicy = game::WeaponFxPolicy::Play;
    bool sourceAirborne = false;
    bool sourceOwnsFullAttitude = false;
    container::String fxListName;
    container::String particleSystemName;
    uint32_t particleSystemLifetimeFrames = 0;
    uint32_t authoredOrder = 0;
    container::String decalTexture;
    math::q32_32 decalRadius{};
    uint32_t decalShadowTypeMask = 0x20u;
    math::q32_32 decalMinimumOpacity{int32_t{1}};
    math::q32_32 decalMaximumOpacity{int32_t{1}};
    uint64_t decalOpacityThrobFrames = 30;
    container::Array<uint8_t, 4> decalColor{0, 0, 0, 0};
    bool decalUsesPlayerColor = true;
    bool decalOnlyVisibleToOwningPlayer = true;
    uint64_t confirmedTick = 0;
};

struct ObjectProjectileGameplayEvent final {
    ObjectProjectileEventKind kind = ObjectProjectileEventKind::Spawned;
    ObjectId projectile = INVALID_OBJECT_ID;
    ObjectId launcher = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    uint32_t sourcePathfindLayer = 0;
    ObjectId target = INVALID_OBJECT_ID;
    uint32_t sourceShotSequence = 0;
    game::WeaponContentId detonationWeapon;
    LogicFixedVec3 position{};
    LogicFixedVec3 sourceVelocity{};
    ObjectPhysicsComponent::Scalar orientationRadians{};
    ObjectPhysicsComponent::Scalar pitchRadians{};
    ObjectPhysicsComponent::Scalar rollRadians{};
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    bool sourceAirborne = false;
    bool sourceOwnsFullAttitude = false;
    uint64_t confirmedTick = 0;
};

// One projectile candidate's authoritative consequences. Presentation keeps
// its own event stream; this transaction preserves per-projectile causal
// closure so OCL/Damage from different missiles are never batch-interleaved.
struct ObjectProjectileGameplayTransaction final {
    ObjectId projectile = INVALID_OBJECT_ID;
    container::Vector<ObjectHistoricBonusWeaponFire> historicBonusWeapons;
    container::Vector<ObjectDamageRequest> damage;
    container::Vector<ObjectProjectileGameplayEvent> events;
};

// Typed RefCode projectile boundary. DumbProjectileBehavior, MissileAIUpdate
// and NeutronMissileUpdate retain independent runtime state; ProjectileObject
// helpers without a ProjectileUpdateInterface are placed at the frozen target
// and left to their own EMP/Neutron update. No family falls back to another.
class ObjectProjectileSystem final {
public:
    void reset() noexcept;

    [[nodiscard]] static std::optional<LogicFixedVec3> resolveBridgeLayerImpact(
        ObjectProjectileComponent& projectile,
        const game::terrain::TerrainLogic& terrain,
        const LogicFixedVec3& destination) noexcept;
    // Returns the stable target-run identity used by ProjectileStream. A
    // target A -> B -> A sequence therefore produces three runs and cannot
    // reconnect after the B projectile disappears from a later snapshot.
    [[nodiscard]] uint32_t resolveProjectileStreamChain(
        const ObjectProjectileSpawnRequest& request, bool streamEnabled);

    // Copies the final frozen projectile recipe on spawn. No raw ModuleData or
    // float content value is parsed at this structural simulation boundary.
    [[nodiscard]] bool initializeObject(ecs::registry& registry, ecs::entity entity,
                                        ObjectId projectile, const game::ObjectArchetype& archetype,
                                        const GameContentSnapshot& content,
                                        const ObjectProjectileSpawnRequest& request,
                                        const game::terrain::TerrainLogic& terrain,
                                        uint32_t logicFramesPerSecond,
                                        math::q32_32 gravityUnitsPerSecondSq =
                                            math::q32_32{
                                                PhysicsSimulationRules::kDefaultGravityUnitsPerSecondSq});

    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const GameContentSnapshot& content, const ObjectSpatialIndex* spatialIndex,
                const PlayerRegistry* players, const game::terrain::TerrainLogic& terrain,
                uint32_t logicFramesPerSecond, uint64_t confirmedTick);

    [[nodiscard]] container::Vector<ObjectProjectileEvent> takeEvents();
    [[nodiscard]] container::Vector<ObjectProjectileGameplayTransaction>
    takeGameplayTransactions();

private:
    void trackActiveProjectile(ObjectId object);

    struct ProjectileStreamOwnerKey final {
        ObjectId launcher = INVALID_OBJECT_ID;
        game::WeaponContentId weapon;
        uint32_t ownerGeneration = 0;
        uint8_t slot = 0;

        [[nodiscard]] bool operator==(
            const ProjectileStreamOwnerKey&) const noexcept = default;
    };

    struct ProjectileStreamOwnerKeyHash final {
        [[nodiscard]] size_t operator()(
            const ProjectileStreamOwnerKey& key) const noexcept {
            const uint64_t pair =
                (static_cast<uint64_t>(key.launcher.value) << 32u) |
                static_cast<uint64_t>(key.weapon.value);
            return std::hash<uint64_t>{}(
                pair ^ (static_cast<uint64_t>(key.ownerGeneration) << 7u) ^
                (static_cast<uint64_t>(key.slot) << 17u));
        }
    };

    struct ProjectileStreamTargetState final {
        ObjectId target = INVALID_OBJECT_ID;
        LogicFixedVec3 position{};
        uint32_t chainIdentity = 0;
        bool initialized = false;
    };

    container::Vector<ObjectProjectileEvent> m_events;
    container::Vector<ObjectProjectileGameplayTransaction>
        m_gameplayTransactions;
    // Production creation registers each typed projectile once. Update keeps
    // this vector in ObjectId order and removes terminal/missing entries in
    // place, avoiding a complete ECS view collection and sort every tick.
    container::Vector<ObjectId> m_activeProjectileIds;
    // Serialized update() scratch shared by collision broad phase and later
    // detonation AoE only after the former has finished consuming it.
    container::Vector<ObjectId> m_collisionCandidateScratch;
    bool m_activeProjectileStoreInitialized = false;
    container::HashMap<ProjectileStreamOwnerKey,
                       ProjectileStreamTargetState,
                       ProjectileStreamOwnerKeyHash>
        m_projectileStreamTargets;
};

} // namespace engine
