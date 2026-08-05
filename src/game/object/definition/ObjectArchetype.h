#pragma once

#include "ThingObjectRecipe.h"
#include "game/object/definition/ObjectKindOf.h"
#include "math/fixed/q32_32.h"

namespace engine {
struct ObjectContainmentPlan;
struct ObjectCombatInitializationPlan;
struct ObjectProjectilePlan;
}

namespace engine::ai {
enum class ObjectAIOrderCapability : uint8_t;
enum class AIRecipeId : uint8_t;
}

namespace game {

class CombatProfile;
struct ThingTemplate;
struct ObjectAirfieldPlan;
struct ObjectDeathReactionPlan;
struct ObjectOnDeletePlan;
struct ObjectDynamicGeometryPlan;
struct ObjectDynamicShroudPlan;
struct ObjectEnemyNearPlan;
struct ObjectAnimationSteeringPlan;
struct ObjectTacticalPlan;
struct ObjectEconomyPlan;
struct ObjectBuilderPlan;
struct ObjectRebuildHolePlan;
struct ObjectCheckpointPlan;
struct ObjectCleanupHazardPlan;
struct ObjectMinefieldPlan;
struct ObjectNeutronMissileSlowDeathPlan;
struct ObjectCountermeasuresPlan;
struct ObjectSmartBombPlan;
struct ObjectStickyBombPlan;
struct ObjectWaveGuidePlan;
struct ObjectSpyVisionPlan;
struct ObjectSpecialPowerPlan;
struct ObjectMissileLauncherBuildingPlan;
struct ObjectParticleUplinkCannonPlan;
struct ObjectSupplyWarehouseCripplingPlan;
struct ObjectTransitionDamageFxPlan;
struct ObjectBoneFxPlan;
struct ObjectBridgeRailPlan;
struct ObjectSpawnSlavePlan;
struct ObjectEmpPlan;
struct ObjectLeafletDropPlan;
struct ObjectStealthPlan;
struct ObjectStealthDetectorPlan;
struct ObjectGrantStealthPlan;
struct ObjectAutoDepositPlan;
struct ObjectAutoHealPlan;
struct ObjectBaseRegeneratePlan;
struct ObjectCrateCollidePlan;
struct ObjectCreatePlan;
struct ObjectSquishCollidePlan;
struct ObjectFloatPlan;
struct ObjectFireWeaponWhenDamagedPlan;
struct ObjectFireWeaponUpdatePlan;
struct ObjectFireWeaponCollidePlan;
struct ObjectFlammablePlan;
struct ObjectFireSpreadPlan;
struct ObjectFireOclAfterCooldownPlan;
struct ObjectHeightDiePlan;
struct ObjectLifetimePlan;
struct ObjectOclUpdatePlan;
struct ObjectOverchargePlan;
struct ObjectPoisonedPlan;
struct ObjectProductionExitPlan;
struct ObjectProductionPlan;
struct ObjectRadiusDecalPlan;
struct ObjectTechBuildingPlan;
struct ObjectUpgradePlan;
struct ObjectWeaponBonusUpdatePlan;
struct ObjectPhysicsPlan;
struct ObjectAIBehaviorPlan;

enum class ObjectRecipeDiagnosticSeverity : uint8_t {
    Warning,
    Error,
};

struct ObjectRecipeDiagnostic final {
    ObjectRecipeDiagnosticSeverity severity = ObjectRecipeDiagnosticSeverity::Warning;
    container::String message;
    container::String sourcePath;
    uint32_t sourceLine = 0;
};

// Immutable output of the legacy object-recipe compiler.  The public
// ThingTemplate value remains useful to old data callers, while GameSession
// and ECS spawn consume this shared immutable boundary so a live match never
// depends on ThingFactory's mutable lookup storage.
struct ObjectArchetype final {
    container::String name;
    ThingTemplate templateData;
    // Session-immutable simulation projection. ThingTemplate exposes only
    // frozen gameplay values; authoring Reals stay in ThingFactory records.
    math::q32_32 sightRangeFixed{};
    math::q32_32 shroudClearingRangeFixed{};
    ObjectKindOfMask kindOfMask{};
    container::SharedPtr<const ObjectAIBehaviorPlan> aiBehaviorPlan;
    // ArmorSet/WeaponSet is compiled as a pointer-free, immutable profile.
    // The archetype owns the shared handle so every ECS entity spawned from
    // it sees the same authored selection rules without reading INI blocks.
    container::SharedPtr<const CombatProfile> combatProfile;
    // TurretAI and PointDefenseLaserUpdate are compiled beside the combat
    // profile so live object creation only copies fixed values and resolves
    // stable content IDs.
    container::SharedPtr<const engine::ObjectCombatInitializationPlan>
        combatInitializationPlan;
    // PhysicsBehavior's final inherited recipe is frozen once per archetype.
    // Live components copy Q32.32 state from this plan and never parse raw
    // ModuleData during a production spawn.
    container::SharedPtr<const ObjectPhysicsPlan> physicsPlan;
    // Projectile flight/helper modules are compiled with the archetype. A
    // live projectile copies this fixed typed plan and never reparses raw
    // ModuleData during structural creation.
    container::SharedPtr<const engine::ObjectProjectilePlan> projectilePlan;
    // Airport, runway, flight-deck and aircraft-control module recipes are
    // frozen as one group so JetAI/Chinook/Spectre state machines consume a
    // single stable ECS contract instead of legacy module interfaces.
    container::SharedPtr<const ObjectAirfieldPlan> airfieldPlan;
    // Immutable DieMux projection compiled from the *final* inherited
    // module recipe. Runtime entities share this handle; no confirmed frame
    // reparses module strings or creates legacy DieModule objects.
    container::SharedPtr<const ObjectDeathReactionPlan> deathReactionPlan;
    // Object::onDestroy invokes module onDelete callbacks in final authored
    // order. Runtime consumes this enum-only table and never compares module
    // class strings at a confirmed lifecycle boundary.
    container::SharedPtr<const ObjectOnDeletePlan> onDeletePlan;
    // TransitionDamageFX is a Damage callback, not a Die action. Its state
    // tables and damage masks are compiled independently so every Body state
    // transition can emit value commands without scanning raw ModuleData.
    container::SharedPtr<const ObjectTransitionDamageFxPlan>
        transitionDamageFxPlan;
    // BoneFXDamage is the Body-state bridge for one or more BoneFXUpdate
    // recipes. Timers remain per entity; parsed state tables are shared.
    container::SharedPtr<const ObjectBoneFxPlan> boneFxPlan;
    // Bridge/Rail keeps map structures, track motion and railed-transport
    // recipes in one immutable island. ObjectBridgeSystem and the shared
    // containment boundary consume these rules only at confirmed ticks.
    container::SharedPtr<const ObjectBridgeRailPlan> bridgeRailPlan;
    // Spawn/Slave/Mob/Hive immutable relationship and formation recipe.
    container::SharedPtr<const ObjectSpawnSlavePlan> spawnSlavePlan;
    // AutoDepositUpdate keeps immutable income/capture rules here while each
    // live entity owns only its confirmed-tick deadlines and one-shot arm.
    container::SharedPtr<const ObjectAutoDepositPlan> autoDepositPlan;
    // Shared immutable AutoHeal recipe. Mutable activation/timer state is
    // attached to each ECS object by ObjectAutoHealSystem at spawn.
    container::SharedPtr<const ObjectAutoHealPlan> autoHealPlan;
    // BaseRegenerateUpdate's module-local recipe is intentionally tiny: its
    // rate/delay live in the session-frozen GameData rules, while each entity
    // owns only sleep state for these final recipe occurrences.
    container::SharedPtr<const ObjectBaseRegeneratePlan> baseRegeneratePlan;
    // Four stock crate pickup classes share this immutable Collide recipe.
    // The runtime component is attached only to actual crate objects.
    container::SharedPtr<const ObjectCrateCollidePlan> crateCollidePlan;
    // Shared immutable Contain-family recipe for Open/Cave/Transport/
    // Garrison/Heal/Tunnel/Overlord/Helix/Parachute style modules. Live
    // passenger state remains in ObjectContainmentComponent by stable id.
    container::SharedPtr<const engine::ObjectContainmentPlan> containmentPlan;
    // PoisonedBehavior stores armor-adjusted repeated-damage state per live
    // entity, while all duration fields are frozen here.
    container::SharedPtr<const ObjectPoisonedPlan> poisonedPlan;
    // OverchargeBehavior is toggled by command/AI/script producers. Active
    // runtime instances add EnergyBonus, hold power rods extended, and emit
    // fixed PENALTY damage through the central Body barrier.
    container::SharedPtr<const ObjectOverchargePlan> overchargePlan;
    // WeaponBonusUpdate freezes its aura filters/timing. Sources and targets
    // retain only compact confirmed-tick runtime components.
    container::SharedPtr<const ObjectWeaponBonusUpdatePlan> weaponBonusUpdatePlan;
    // Damage-triggered reaction/continuous private weapons share immutable
    // names, filters and UpgradeMux data here; per-object ammo/cooldown stays
    // in a sparse ECS runtime.
    container::SharedPtr<const ObjectFireWeaponWhenDamagedPlan>
        fireWeaponWhenDamagedPlan;
    // Periodic self-position weapons keep their immutable timing/name recipe
    // here and only ammo/deadlines on each ECS entity.
    container::SharedPtr<const ObjectFireWeaponUpdatePlan> fireWeaponUpdatePlan;
    // Contact-triggered weapons share immutable filters and retain only the
    // FireOnce/shot sequence state per entity.
    container::SharedPtr<const ObjectFireWeaponCollidePlan>
        fireWeaponCollidePlan;
    // SquishCollide has no authored payload, but its presence marks an object
    // as the victim-side participant in legacy infantry-vs-crusher contact.
    container::SharedPtr<const ObjectSquishCollidePlan> squishCollidePlan;
    // Fire damage thresholds, propagation and cooldown-triggered OCLs share
    // one sparse confirmed-frame system but retain their original module
    // recipes and author ordering as separate immutable plans.
    container::SharedPtr<const ObjectFlammablePlan> flammablePlan;
    container::SharedPtr<const ObjectFireSpreadPlan> fireSpreadPlan;
    container::SharedPtr<const ObjectFireOclAfterCooldownPlan>
        fireOclAfterCooldownPlan;
    // EMPUpdate keeps its authored pulse/filter/visual recipe immutable.
    // Each effect object owns only confirmed-tick clocks and the sampled
    // target scale; affected objects use the shared Disabled reason store.
    container::SharedPtr<const ObjectEmpPlan> empPlan;
    // LeafletDropBehavior shares the common Disabled writer but retains its
    // enemy infantry/vehicle pulse and non-terminal Die callback recipe.
    container::SharedPtr<const ObjectLeafletDropPlan> leafletDropPlan;
    // StealthUpdate is a single-interface fixed-tick controller. Its mutable
    // re-stealth/detection clocks live only on entities which own this plan.
    container::SharedPtr<const ObjectStealthPlan> stealthPlan;
    // Detector pulse/filter data is independent of whether this object can
    // itself stealth. Runtime owns only enable/scheduling state.
    container::SharedPtr<const ObjectStealthDetectorPlan> stealthDetectorPlan;
    // GPS Scrambler marker expands a fixed-point grant radius and then
    // destroys itself; every live marker shares this immutable recipe.
    container::SharedPtr<const ObjectGrantStealthPlan> grantStealthPlan;
    // DynamicShroudClearingRangeUpdate keeps one immutable timing/decal
    // recipe per authored occurrence. Live ranges and clocks remain sparse
    // ECS state and never mutate this session-frozen archetype.
    container::SharedPtr<const ObjectDynamicShroudPlan> dynamicShroudPlan;
    // DynamicGeometryInfoUpdate and its Firestorm subclass share fixed-point
    // interpolation rules here. Per-object clocks, current radii and
    // persistent-effect latches remain sparse runtime state.
    container::SharedPtr<const ObjectDynamicGeometryPlan>
        dynamicGeometryPlan;
    // EnemyNearUpdate is a tiny visual-state scanner. Runtime retains only
    // per-object scan clocks and the last near/not-near bit.
    container::SharedPtr<const ObjectEnemyNearPlan> enemyNearPlan;
    // AnimationSteeringUpdate converts confirmed yaw changes into the four
    // legacy steering animation conditions without exposing renderer state.
    container::SharedPtr<const ObjectAnimationSteeringPlan>
        animationSteeringPlan;
    container::SharedPtr<const ObjectTacticalPlan> tacticalPlan;
    // Economy/dock/workforce family.  Stage 1 executes AutoFindHealing and
    // RepairDock directly; the same frozen presence table records the rest
    // of the group 6 modules for the construction/supply follow-up.
    container::SharedPtr<const ObjectEconomyPlan> economyPlan;
    // DozerAIUpdate and WorkerAIUpdate share one modern construction/repair
    // controller; Worker composes this plan with the economy supply runtime.
    container::SharedPtr<const ObjectBuilderPlan> builderPlan;
    container::SharedPtr<const ObjectRebuildHolePlan> rebuildHolePlan;
    // CheckpointUpdate owns its immutable scan interval here; proximity bits,
    // door transition and mutable gate footprint remain sparse ECS state.
    container::SharedPtr<const ObjectCheckpointPlan> checkpointPlan;
    // CleanupHazardUpdate shares one immutable scan/weapon recipe per final
    // archetype; target clocks and CleanupArea state remain sparse ECS data.
    container::SharedPtr<const ObjectCleanupHazardPlan> cleanupHazardPlan;
    // GenerateMinefieldBehavior, MinefieldBehavior and DemoTrapUpdate share
    // one immutable trap/mine recipe island. Per-object counters, stable
    // detonator identities and mode clocks remain sparse ECS state.
    container::SharedPtr<const ObjectMinefieldPlan> minefieldPlan;
    container::SharedPtr<const ObjectNeutronMissileSlowDeathPlan>
        neutronMissileSlowDeathPlan;
    // CountermeasuresBehavior freezes its UpgradeMux, Q32.32 evasion/volley
    // scalars and authored timing here. Live flare IDs/deadlines are sparse.
    container::SharedPtr<const ObjectCountermeasuresPlan>
        countermeasuresPlan;
    container::SharedPtr<const ObjectSmartBombPlan> smartBombPlan;
    container::SharedPtr<const ObjectStickyBombPlan> stickyBombPlan;
    // WaveGuideUpdate freezes its Q32.32 crest/damage recipe.  Waypoint
    // topology and water impulses remain session-local TerrainLogic state.
    container::SharedPtr<const ObjectWaveGuidePlan> waveGuidePlan;
    // SpyVisionUpdate freezes UpgradeMux, self-powered cadence and KindOf
    // selection here. Live activation/disable clocks remain sparse ECS state.
    container::SharedPtr<const ObjectSpyVisionPlan> spyVisionPlan;
    // All SpecialPowerInterface modules share one recharge/science runtime.
    // Effect-specific payloads (currently SpyVision) remain typed rules in
    // this plan while sabotage can restart every occurrence uniformly.
    container::SharedPtr<const ObjectSpecialPowerPlan> specialPowerPlan;
    container::SharedPtr<const ObjectMissileLauncherBuildingPlan>
        missileLauncherBuildingPlan;
    // ParticleUplinkCannonUpdate keeps the complete authored beam/damage/
    // presentation recipe immutable; live charge and swath clocks are sparse.
    container::SharedPtr<const ObjectParticleUplinkCannonPlan>
        particleUplinkCannonPlan;
    // Warehouse damage callbacks and self-heal clocks are live ECS state;
    // authored suppression/delay/amount values remain immutable here.
    container::SharedPtr<const ObjectSupplyWarehouseCripplingPlan>
        supplyWarehouseCripplingPlan;
    // FloatUpdate retains only its per-occurrence Enabled switch. Water
    // lookup and position authority are live ECS concerns.
    container::SharedPtr<const ObjectFloatPlan> floatPlan;
    // HeightDieUpdate freezes every module-local threshold/delay occurrence.
    // Live entities retain only direction, delay and one-shot state.
    container::SharedPtr<const ObjectHeightDiePlan> heightDiePlan;
    // Shared immutable LifetimeUpdate/DeletionUpdate recipe. The runtime
    // stores only armed absolute deadlines and never reparses module text.
    container::SharedPtr<const ObjectLifetimePlan> lifetimePlan;
    // OCLUpdate's immutable timers and faction-specific recipe names. Each
    // entity retains only resolved content IDs and confirmed-tick deadlines.
    container::SharedPtr<const ObjectOclUpdatePlan> oclUpdatePlan;
    // Factory and default-exit content are frozen separately from the live
    // per-producer queue, so cash/commands never reparse CommandSet/INI data.
    container::SharedPtr<const ObjectProductionPlan> productionPlan;
    container::SharedPtr<const ObjectProductionExitPlan> productionExitPlan;
    // RadiusDecalUpdate has no authored fields of its own; it is a sparse
    // host for delivery decals produced by OCL/AI/special-power systems.
    container::SharedPtr<const ObjectRadiusDecalPlan> radiusDecalPlan;
    // TechBuildingBehavior and BeaconClientUpdate are map-object boundaries:
    // owner capture state, pulse FX, beacon smoke and radar pulse requests
    // are frozen here while live entities keep only sparse wake state.
    container::SharedPtr<const ObjectTechBuildingPlan> techBuildingPlan;
    // Frozen UpgradeMux subset and PowerPlantUpdate rod recipe. Runtime
    // entities retain only compact activation/state components.
    container::SharedPtr<const ObjectUpgradePlan> objectUpgradePlan;
    // Final CreateModule author order. LockWeaponCreate currently contributes
    // the first typed game-side onBuildComplete operation; later Create
    // families extend this shared plan instead of adding virtual modules.
    container::SharedPtr<const ObjectCreatePlan> createPlan;
    // CrateCollide's original eligibility check asks whether the picker owns
    // an AIUpdate interface. Freeze that capability bit once instead of
    // scanning the final module recipe on every overlap.
    bool hasAiUpdate = false;
    // Lifecycle membership must not rediscover the concrete AI recipe from
    // retained ModuleData. Compile the generic runtime's initial order mask
    // beside hasAiUpdate; specialized recipes remain capability-off until
    // their owning adapter explicitly enables a route.
    engine::ai::AIRecipeId aiRecipe{};
    engine::ai::ObjectAIOrderCapability initialAiOrderCapabilities{};
    uint64_t recipeFingerprint = 0;
    // True means the recipe retains at least one opaque third-party module,
    // or an opaque copied-parent relationship could affect a later module
    // family compiler. Stock ModuleFactory classes are resolved through the
    // immutable catalog before this archetype is published; unsupported data
    // remains intact rather than receiving a guessed interface mask.
    bool requiresInterfaceResolution = false;
    container::Vector<ObjectRecipeDiagnostic> diagnostics;
};

} // namespace game
