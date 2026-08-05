#pragma once

#include "core/container/container_types.h"

#include "game/player/PlayerList.h"
#include "core/ecs/ObjectId.h"
#include "game/base/DamageTypes.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "math/fixed/q32_32.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace game {
struct ObjectArchetype;
struct ThingTemplate;
}

namespace engine {

// Stable simulation identity is deliberately a component rather than an ECS
// entity value. EnTT entities are transient storage handles; ObjectId remains
// valid in commands, scripts, snapshots and lifecycle events until the object
// is destroyed, and is never reused during a session.
struct ObjectIdentityComponent {
    ObjectId id = INVALID_OBJECT_ID;
};

// Sparse, per-object replacement for Object::setVisionRange() and
// Object::setShroudClearingRange().  Most objects keep using their immutable
// ThingTemplate values and therefore do not carry this component.  Runtime
// conversions (pilot-to-vehicle, car bomb, hijack, etc.) can project the
// source object's effective ranges onto the surviving target without
// mutating the shared archetype used by every object of that type.
struct ObjectVisionRangeOverrideComponent final {
    using Scalar = math::q32_32;

    Scalar visionRange{};
    Scalar shroudClearingRange{};
};

enum class ObjectCreationOrigin : uint8_t {
    Map,
    Scenario,
    PlayerCommand,
    Script,
    System,
    Production,
};

enum class ObjectLifecyclePhase : uint8_t {
    Alive,
    PendingDestroy,
};

// Structural changes are centralized in ObjectLifecycle. A requested destroy
// marks this component first, then a deterministic lifecycle flush removes
// the ECS entity and emits the destroyed event. Systems can therefore avoid
// retaining an entity while another system invalidates its component storage.
struct ObjectLifecycleComponent {
    ObjectCreationOrigin origin = ObjectCreationOrigin::System;
    ObjectLifecyclePhase phase = ObjectLifecyclePhase::Alive;
    uint64_t createdAtTick = 0;
};

// One-tick script-visible projection of AIUpdate::m_completedWaypoint.  AI
// settles a waypoint route after scripts have already run for the confirmed
// frame, so conditions observe this marker on the next script pass only.
// The terminal map waypoint is stable; no AI handle or ECS entity escapes.
struct ObjectWaypointCompletionComponent final {
    uint32_t terminalWaypointId = std::numeric_limits<uint32_t>::max();
    uint64_t waypointGraphRevision = 0;
    uint64_t completedAtTick = 0;
};

// A unit produced by a factory retains only stable value provenance.  It is
// intentionally separate from projectile launch ownership: producer-linked
// production, UI history and future Create/onBuildComplete consumers must
// never reinterpret a weapon's transient source as a factory relationship.
struct ObjectProducedByComponent final {
    ObjectId producer = INVALID_OBJECT_ID;
    uint32_t productionId = 0;
    uint32_t quantityIndex = 0;
};

// General Object::setProducer relation used by OCL/debris/payload creation.
// It is intentionally separate from factory production provenance: the
// producer may already be pending destruction and no EnTT handle is retained.
struct ObjectProducerComponent final {
    ObjectId producer = INVALID_OBJECT_ID;
};

// Modern value replacement for Object::m_layer.  Layer zero is the terrain
// heightfield; nonzero values identify TerrainLogic-owned bridge/wall
// surfaces.  It belongs to the object rather than Physics or AI because
// locomotion, projectiles, containment exits and death creation all share
// the same durable gameplay fact.
struct ObjectTerrainLayerComponent final {
    uint32_t pathfindLayer = 0;
    uint64_t revision = 1;
    uint64_t lastChangedTick = 0;

    [[nodiscard]] bool assign(uint32_t layer, uint64_t confirmedTick) noexcept {
        if (pathfindLayer == layer) return false;
        pathfindLayer = layer;
        if (revision != std::numeric_limits<uint64_t>::max()) ++revision;
        lastChangedTick = confirmedTick;
        return true;
    }
};

// ParkingPlace/FlightDeck reservation freezes the carrier and authored deck
// offset into the aircraft.  Physics can therefore sample the correct deck
// surface without retaining an Airfield behavior pointer or inspecting raw
// ModuleData during a confirmed tick.
struct ObjectCarrierDeckComponent final {
    using Scalar = math::q32_32;

    ObjectId carrier = INVALID_OBJECT_ID;
    Scalar heightOffset{};
};

// StructureBody's only state beyond ActiveBody is the stable identity of the
// object that constructed it.  Keep this relation separate from factory
// production provenance: a constructor may disappear while the completed
// structure remains, and neither side retains an ECS entity handle.
struct ObjectConstructedByComponent final {
    ObjectId constructorObject = INVALID_OBJECT_ID;
};

// A compact, module-neutral projection of the legacy BodyModule state. It is
// pure data: HealthSystem owns transactions, damage callbacks and the
// deferred death request; no component mutator destroys an entity directly.
//
// Authoring still enters through legacy floating-point INI values, but live
// Body state is Q32.32. This prevents platform/compiler-specific float
// accumulation from deciding threshold transitions, armor clipping or a
// same-frame kill. Presentation converts these values only at extraction.
struct ObjectHealthComponent {
    using Scalar = math::q32_32;

    Scalar currentFixed{int32_t{1}};
    Scalar previousFixed{int32_t{1}};
    Scalar maximumFixed{int32_t{1}};
    Scalar initialFixed{int32_t{1}};
    Scalar subdualDamageFixed{};
    Scalar subdualDamageCapFixed{};
    Scalar subdualDamageHealAmountFixed{};
    Scalar secondLifeMaximumHealthFixed{};
    // Absolute confirmed tick at which the next helper-equivalent subdual
    // recovery pulse occurs. Zero means no recovery is armed.
    uint64_t nextSubdualRecoveryTick = 0;

    uint32_t subdualDamageHealIntervalMilliseconds = 0;
    ObjectBodyDamageState damageState = ObjectBodyDamageState::Pristine;
    // ActiveBody retains a preferred recent DamageInfo across the current
    // and previous logic frame. TransitionDamageFX filters against this type,
    // not necessarily the request which happened to cross the HP threshold.
    ObjectId lastDamageSource = INVALID_OBJECT_ID;
    // DamageInfo snapshots source template/player at transaction time so
    // script conditions remain valid after the attacker is destroyed. A
    // shared immutable archetype avoids a per-hit string allocation while
    // keeping the sparse historical identity detached from an ECS entity.
    PlayerId lastDamageSourcePlayer = INVALID_PLAYER_ID;
    ::container::SharedPtr<const game::ObjectArchetype>
        lastDamageSourceArchetype;
    game::DamageType lastDamageType = game::DamageType::EXPLOSION;
    uint64_t lastDamageTick = 0;
    bool hasLastDamageInfo = false;
    // ActiveBody owns DamageFX throttling per victim. A repeated hit of the
    // same visual damage type is suppressed until this confirmed tick;
    // changing type bypasses the previous type's window, matching ZH.
    game::DamageType lastDamageFxType = game::DamageType::EXPLOSION;
    uint64_t nextDamageFxTick = 0;
    bool hasDamageFxThrottle = false;
    // InactiveBody rejects all ordinary damage. ImmortalBody never reaches
    // zero. HighlanderBody can only die from UNRESISTABLE damage. These are
    // data flags rather than a new virtual Body hierarchy.
    bool acceptsDamage = true;
    // Runtime counterpart of BodyModuleInterface::setIndestructible().  It is
    // distinct from InactiveBody: the Body remains alive/queryable but every
    // damage attempt, including scripted force-kill, is ignored until the
    // flag is cleared. Delete still uses the lifecycle path and bypasses Body.
    bool indestructible = false;
    bool clampsToOneHealth = false;
    // Sparse runtime behaviors may impose a smaller absolute Body floor
    // without pretending to be ImmortalBody. MinefieldBehavior uses the
    // original 0.1 HP floor while it can regenerate; zero means no runtime
    // floor. The Body transaction combines this with the immutable one-HP
    // clamp instead of letting modules edit current health after death.
    Scalar minimumHealthFloorFixed{};
    bool onlyUnresistableCanKill = false;
    // UndeadBody has one explicitly authored recovery transition.  It is
    // stored on the live Body projection instead of a virtual Body subclass
    // so damage remains an ordered ECS transaction.  `secondLifeActive`
    // makes the transition one-shot; later lethal damage follows the normal
    // death path.
    bool hasSecondLife = false;
    bool secondLifeActive = false;
    // Modern projection of DISABLED_SUBDUED for normal objects. Movement and
    // weapon systems consume this flag; projectile-specific jamming remains
    // owned by the future MissileAI/Projectile controller.
    bool subdued = false;
    bool effectivelyDead = false;
    // InactiveBody starts effectively dead but still accepts exactly one
    // UNRESISTABLE Die path. Track that separately from health so a repeated
    // damage request cannot duplicate structural destruction.
    bool terminalDeathIssued = false;
};

enum class ObjectPanelFlag : uint8_t {
    Enabled,
    Powered,
    Indestructible,
    Unsellable,
    Selectable,
    AiRecruitable,
    PlayerTargetable,
};

// Persistent flags authored by the legacy Object Properties panel or changed
// through UNIT/TEAM_AFFECT_OBJECT_PANEL_FLAGS. Enabled/Powered/Selectable and
// Indestructible have dedicated authoritative components; these remaining
// policy values are kept together for Sell, Player command and Player-AI
// consumers without recreating Object's packed script-status byte.
struct ObjectScriptPanelPolicyComponent final {
    bool unsellable = false;
    bool aiRecruitable = true;
    bool playerTargetable = false;
    uint64_t revision = 0;
};

// Confirmed text displayed above one Drawable.  Retail currently writes this
// slot from MSG_SET_BEACON_TEXT, but keeping it object-scoped preserves the
// Drawable contract for future/modded producers without retaining a GUI
// widget or renderer string object in simulation state.  The shipped beacon
// edit control accepts at most 64 Unicode code points.
struct ObjectDrawableCaptionComponent final {
    static constexpr size_t MaximumCodePoints = 64;

    container::String text;
    uint64_t lastChangedTick = 0;
    uint64_t revision = 0;
};

// Session-owned sale transaction attached to the structure being
// deconstructed. RefCode resolves the controlling player and build-cost
// modifiers at completion, so this component owns timing only.
struct ObjectSaleComponent final {
    uint64_t startedTick = 0;
    uint64_t completionTick = 0;
    uint64_t revision = 0;

    [[nodiscard]] uint64_t totalFrames() const noexcept {
        return completionTick >= startedTick
            ? completionTick - startedTick + 1u : 1u;
    }

    [[nodiscard]] uint64_t scaffoldAnimationFrames() const noexcept {
        // BuildAssistant applies TOTAL_FRAMES_TO_SELL_OBJECT / 2 to the
        // Drawable at both sale admission and the SOLD edge.  The complete
        // TD sale window is 6 seconds (1.5 rise + 3.0 body descent + 1.5
        // scaffold sink), so one quarter of this component's duration is the
        // same authored 1.5-second animation loop.
        return std::max<uint64_t>(1, totalFrames() / 4u);
    }

    [[nodiscard]] math::q32_32 constructionPercent(
        uint64_t confirmedTick) const noexcept {
        const uint64_t total = totalFrames();
        const uint64_t scaffold = scaffoldAnimationFrames();
        const uint64_t descent = std::max<uint64_t>(1, total / 2u);
        const uint64_t elapsed = confirmedTick > startedTick
            ? confirmedTick - startedTick : 0u;
        // BuildAssistant::sellObject starts at 99.9%, publishes the two
        // construction model conditions immediately, then leaves the height
        // fixed while the scaffold rises.  Its update at elapsed==scaffold is
        // the first 100/descent decrement; keeping that inclusive edge is what
        // makes the SOLD transition and the final -50% scaffold sink land on
        // the same frames as RefCode.
        if (elapsed < scaffold)
            return math::q32_32::from_fraction(999, 10);
        const uint64_t steps = std::min<uint64_t>(
            elapsed - scaffold + 1u,
            static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max() / 1000));
        const int64_t remainingNumerator =
            static_cast<int64_t>(descent) * 999 -
            static_cast<int64_t>(steps) * 1000;
        return math::q32_32::from_fraction(
            remainingNumerator,
            static_cast<int64_t>(descent) * 10);
    }

    [[nodiscard]] bool soldVisualActive(
        uint64_t confirmedTick) const noexcept {
        return constructionPercent(confirmedTick) <= math::q32_32{};
    }
};

// GeometryInfo is gameplay data in the original engine. Keep its authored
// shape and derived radii beside the entity so selection/spatial systems do
// not depend on render assets or perform template/string lookups per query.
enum class ObjectGeometryShape : uint8_t {
    Sphere,
    Cylinder,
    Box,
};

struct ObjectGeometryComponent {
    ObjectGeometryShape shape = ObjectGeometryShape::Sphere;
    bool isSmall = true;
    // ObjectLifecycle quantizes authored geometry once. Presentation converts
    // these immutable session values only while extracting a frame.
    math::q32_32 majorRadiusFixed{int32_t{1}};
    math::q32_32 minorRadiusFixed{int32_t{1}};
    math::q32_32 heightFixed{int32_t{1}};
    math::q32_32 boundingCircleRadiusFixed{int32_t{1}};
    math::q32_32 boundingSphereRadiusFixed{int32_t{1}};
};

// CreateCrateDie marks only the object it actually created.  A template name
// or KindOf test cannot recover this fact: the same crate template may be
// spawned by maps/OCL without the original Drawable's EXJunkCrate marker.
// The payload is confirmed, value-only provenance consumed by presentation.
struct ObjectCrateTerrainDecalComponent final {
    uint64_t createdAtTick = 0;
};

// AIUpdateInterface::setIgnoreCollisionTime expressed as a sparse confirmed
// deadline. Transport exits use one legacy logic second so stacked passengers
// can clear the shared exit without physics bouncing them back into it.
struct ObjectTemporaryCollisionIgnoreComponent final {
    uint64_t untilTick = 0;
    ObjectId other = INVALID_OBJECT_ID;
};

// StatusDamageHelper owns exactly one timed status per object. Applying a
// different status clears the previous one; reapplying the same status only
// replaces its deadline.
struct ObjectTimedStatusDamageComponent final {
    uint64_t statusMask = 0;
    uint64_t clearAtTick = 0;
};

// ObjectRepulsorHelper is independent from StatusDamageHelper in RefCode.
// Keep its two-second wake deadline separate so poison/status damage cannot
// overwrite civilian panic propagation (or vice versa).
struct ObjectRepulsorExpiryComponent final {
    uint64_t clearAtTick = 0;
};

enum class ObjectOrderKind : uint8_t {
    Move,
    // Stop is an interruption operation rather than a persistent queue item.
    // OrderExecutor clears the current queue synchronously for this kind, so
    // an unimplemented consumer can never leave it permanently at the head.
    Stop,
    Attack,
    Build,
    CommandButton,
    SpecialPower,
    // Typed tactical wrappers are admitted separately from direct Attack.
    // The first ingress slice is scenario-script Hunt only; no player wire
    // command maps to this family.
    TacticalAttack,
};

enum class ObjectTacticalAttackSubtype : uint8_t {
    None,
    Hunt,
    // Persistent current-position guard.  The confirmed observer freezes the
    // anchor; targetless ingress must not be reinterpreted as Hunt.
    Guard,
    AttackSquad,
    AttackArea,
    // Damage-authority produced assistance order.  It is never inferred
    // from an ordinary Attack payload: the victim/aggressor transaction
    // publishes this explicit subtype for nearby eligible friends.
    GuardRetaliate,
    // Script-only Guard policy that enters the nearest tunnel network.
    GuardTunnelNetwork,
};

// Move remains the queue/wire family.  This subtype describes how its goal is
// produced without exposing an AIStateId through command ingress.  Only the
// script-only waypoint routes and the system-owned production-exit route are
// admitted; player Move and AttackMove retain Direct.
enum class ObjectMoveRouteSubtype : uint8_t {
    Direct,
    WaypointPathIndividuals,
    WaypointPathTeam,
    WaypointPathIndividualsExact,
    WaypointPathTeamExact,
    WanderWaypointPath,
    PanicWaypointPath,
    FollowPath,
    FollowExitProductionPath,
    // Player group Move whose destination lies inside the scaled current
    // group bounds. It remains the same queue order; only the AI state route
    // changes to the stock gather/tighten behavior.
    Tighten,
    MoveAside,
    // Script AI_WANDER_IN_PLACE is a persistent state, not a one-shot MoveTo.
    // Keep it in the Move command family while giving ObjectAI an explicit
    // state route; a later player/script command replaces the same queue head.
    WanderInPlace,
};

[[nodiscard]] constexpr bool isObjectWaypointRouteSubtype(
    ObjectMoveRouteSubtype subtype) noexcept {
    return subtype == ObjectMoveRouteSubtype::WaypointPathIndividuals ||
           subtype == ObjectMoveRouteSubtype::WaypointPathTeam ||
           subtype == ObjectMoveRouteSubtype::WaypointPathIndividualsExact ||
           subtype == ObjectMoveRouteSubtype::WaypointPathTeamExact ||
           subtype == ObjectMoveRouteSubtype::WanderWaypointPath ||
           subtype == ObjectMoveRouteSubtype::PanicWaypointPath;
}

[[nodiscard]] constexpr bool objectWaypointRouteMovesAsTeam(
    ObjectMoveRouteSubtype subtype) noexcept {
    return subtype == ObjectMoveRouteSubtype::WaypointPathTeam ||
           subtype == ObjectMoveRouteSubtype::WaypointPathTeamExact;
}

[[nodiscard]] constexpr bool objectWaypointRouteIsExact(
    ObjectMoveRouteSubtype subtype) noexcept {
    return subtype == ObjectMoveRouteSubtype::WaypointPathIndividualsExact ||
           subtype == ObjectMoveRouteSubtype::WaypointPathTeamExact;
}

// Command ingress is simulation provenance, not a UI/network object.  Weapon
// selection needs the same Player/Script distinction as the original
// AutoChooseSources rule, so retain it explicitly instead of inferring it
// from whether an incidental script ID happens to be zero.
enum class ObjectOrderSource : uint8_t {
    Player,
    Script,
    System,
};

// System-authored intents are still ordinary value commands, but their
// producer is typed so autonomous controllers can distinguish their own
// pursuit from a new player/script order without magic content-name strings.
enum class ObjectOrderSystemPurpose : uint8_t {
    Generic,
    CleanupHazard,
    AutoFindHealing,
    SupplyTruck,
    RailedTransport,
    AssaultTransport,
    PilotFindVehicle,
    DeliverPayload,
    Builder,
    SlaveReturn,
    TacticalAssist,
    SpecialAbility,
    CommandButtonHunt,
    Wander,
    ContainmentExit,
    ProductionExit,
    RepairDock,
    Retaliation,
    // Appended instead of inserted before ContainmentExit: the ECS protocol
    // is mirrored by ObjectAIOrderSystemPurpose, and the existing exit/path
    // purposes already have stable runtime ordinals.
    ContainmentEnter,
    // Script reinforcement transports use ordinary Move ownership, but their
    // terminal edges are structural: unload/activate at the destination and,
    // when authored, return to the spawn origin before deletion.  The Team ID
    // is carried in systemPurposeInstance, avoiding a parallel pointer/state
    // machine outside the deterministic order queue.
    ScenarioReinforcementDeliver,
    ScenarioReinforcementExit,
    MoveAside,
    // Player HIJACK_VEHICLE / CONVERT_TO_CARBOMB / SABOTAGE_BUILDING uses a
    // normal dynamic-target Move, but Movement must retain the reached head
    // until the later same-tick CrateCollide phase consumes the contact.
    IntentionalContact,
    // ObjectCreationList's Attack nugget publishes one position attack on the
    // OCL source after locking the authored WeaponSlot temporarily.  The
    // purpose is explicit so Combat can recognize a producer that selected the
    // slot in the same confirmed tick and must not have that fresh temporary
    // lock erased when the new attack identity is first observed.
    ObjectCreationAttack,
    // Player-level AI planning is an authoritative deterministic producer,
    // not replay/network input. Keep it distinct from player clicks and from
    // object-local TacticalAssist behavior.
    StrategicAI,
    // ParachuteContain::onRemoving chooses Hunt for a skirmish AI rider.
    // Keep that AI-authored transition distinct from CommandButtonHunt and
    // from player-level strategic planning.
    ParachuteLanding,
    // Construction placement owns a new reachable evacuation goal. This is
    // deliberately not MoveAside: MoveAside only rebinds an existing path.
    ConstructionEvacuation,
    // FIRE_WEAPON CommandButtons publish an ordinary Attack after selecting
    // the authored WeaponSlot. Preserve that temporary selection across the
    // first combat observation of the new order identity.
    CommandButtonFireWeapon,
};

// A deterministic intent queue, not a movement/weapon implementation. The
// future locomotor, combat, factory and special-power systems consume this
// state at their own confirmed phase; no UI, network packet or script keeps a
// pointer/EnTT entity in the order.
struct ObjectOrderIntent final {
    ObjectOrderKind kind = ObjectOrderKind::Move;
    // TacticalAttack requires one explicit typed subtype. None is retained as
    // the neutral value for every other order family, preventing a direct
    // Attack or Move from being reinterpreted as Hunt/Guard by payload
    // coincidence.
    ObjectTacticalAttackSubtype tacticalAttackSubtype =
        ObjectTacticalAttackSubtype::None;
    ObjectOrderSource source = ObjectOrderSource::System;
    // Player input stores its issuing player here. Scenario-script orders
    // store the ScriptList's current player only as provenance: it is not an
    // ownership/authorization claim, because RefCode may direct enemy or
    // civilian named objects with CMD_FROM_SCRIPT.
    PlayerId contextPlayer = INVALID_PLAYER_ID;
    uint64_t issuedTick = 0;
    // Player commands use their frame-local GameCommand sequence. Script
    // orders use ScriptEffectHeader::ordinal, so two orders produced by one
    // script in one confirmed tick never collapse to the same source order.
    uint32_t sourceSequence = 0;
    // Nonzero only for a ScriptRuntime-produced order; retained for
    // diagnostics/profiling rather than overloaded into sourceSequence.
    uint32_t sourceScriptId = 0;
    ObjectId targetObject = INVALID_OBJECT_ID;
    math::q32_32 targetX{};
    math::q32_32 targetY{};
    math::q32_32 targetZ{};
    bool hasTargetPosition = false;
    // Build uses the yaw as its placement orientation; SpecialPower preserves
    // it as the command angle consumed by OCL creation. A valid end position
    // remains Build-only and denotes a LINEBUILD anchor; the authoritative
    // session recomputes the tile plan.
    math::q32_32 placementYawRadians{};
    math::q32_32 placementEndX{};
    math::q32_32 placementEndY{};
    math::q32_32 placementEndZ{};
    bool hasPlacementEndPosition = false;
    ::container::String contentName;
    // nullopt keeps an ordinary Attack intent open-ended. FireWeaponPower
    // preserves an explicitly authored zero-shot cap, while positive values
    // advance only after Combat publishes an authoritative firing cycle.
    std::optional<uint32_t> maximumShots;
    uint32_t shotsFired = 0;
    // Preserves AI force-attack intent for script-issued object-target
    // attacks. The Stage-1 direct-hit consumer retains it until future
    // pursuit/retargeting logic can distinguish forced from ordinary attack.
    bool forceAttack = false;
    // Retains the original attack-move distinction while reusing the Move
    // locomotion intent. Opportunity combat may fire without consuming the
    // destination; ordinary Move keeps this clear.
    bool attackMove = false;
    // Typed protocol bit retained after GameCommand becomes a queue intent.
    // contentName remains authored provenance and is never behavior routing.
    bool combatDrop = false;
    ObjectMoveRouteSubtype moveRouteSubtype = ObjectMoveRouteSubtype::Direct;
    // Stable map-scoped waypoint identity. UINT32_MAX is absent; Terrain's
    // legal ID zero is encoded as AIWaypointHandle{1} only at the AI boundary.
    uint32_t waypointStartId = std::numeric_limits<uint32_t>::max();
    // Stable digest of the immutable authored waypoint graph. It prevents a
    // restored route from silently resolving the same numeric ID in a
    // different graph.
    uint64_t waypointGraphRevision = 0;
    // Present only for the two as-team waypoint variants. The stable live
    // Team handle lets the SoA coordinator share progress without retaining
    // ObjectTeamRegistry or ECS pointers in the AI hot path.
    ObjectTeamId waypointTeam = INVALID_OBJECT_TEAM_ID;
    // ZH freezes each movable, non-held member's XY displacement from the
    // AIGroup centre when an AS_TEAM waypoint state enters. Store that
    // per-object fixed offset with the admitted order so the later SoA bridge
    // never has to revisit a mutable Team/ECS roster or use floating point.
    math::q32_32 waypointGroupOffsetX{};
    math::q32_32 waypointGroupOffsetY{};
    // Zero means FAST_AS_POSSIBLE. AS_TEAM freezes the slowest admitted
    // member's undamaged locomotor speed, matching AIGroup::getSpeed().
    math::q32_32 waypointGroupSpeed{};
    uint64_t groupPathId = 0;
    uint32_t groupPathMemberOrdinal = 0;
    uint32_t groupPathMemberCount = 0;
    math::q32_32 groupPathStartX{};
    math::q32_32 groupPathStartY{};
    math::q32_32 groupPathStartZ{};
    math::q32_32 groupPathOffsetX{};
    math::q32_32 groupPathOffsetY{};
    // Hunt policy is value-only ingress data. The future tactical owner may
    // use these independently; they do not imply an object/position target.
    bool allArmyHunt = false;
    bool useTeamCommonTarget = false;
    // ZH player Guard mode bits. They are explicit queue data so replay and
    // detached SoA admission never infer behavior from a CommandButton name.
    bool guardWithoutPursuit = false;
    bool guardFlyingOnly = false;
    // Script-only tactical target domains. AttackSquad binds one live Team;
    // AttackArea binds one immutable map PolygonTrigger. Neither is inferred
    // from targetObject/targetPosition, so the queue remains unambiguous.
    ObjectTeamId tacticalTargetTeam = INVALID_OBJECT_TEAM_ID;
    uint32_t tacticalTargetAreaId = std::numeric_limits<uint32_t>::max();
    uint64_t tacticalTargetRevision = 0;
    ObjectOrderSystemPurpose systemPurpose =
        ObjectOrderSystemPurpose::Generic;
    // Disambiguates multiple occurrences of the same autonomous module in a
    // modded final recipe.  Zero is the first authored occurrence.
    uint32_t systemPurposeInstance = 0;
};

struct ObjectOrderQueueComponent final {
    // Retail's dynamic waypoint goal uses a vector and the W3D presentation
    // displays at most 512 nodes.  Use the same practical ceiling for the
    // deterministic queue so a Shift/waypoint route is not silently truncated at the
    // old TD-only 64-order limit.
    static constexpr size_t MaximumQueuedOrders = 512;

    ::container::Vector<ObjectOrderIntent> orders;
    uint64_t revision = 0;
    // Changes only at player/scenario command admission, including Stop.
    // Autonomous systems use it to observe an explicit override even when an
    // immediate command clears the queue and leaves no persistent intent.
    uint64_t externalRevision = 0;
    // Equals externalRevision only when the latest admitted external command
    // used replacement (non-Shift) semantics. Appending or deleting one
    // waypoint advances externalRevision without changing this stamp, so an
    // active Builder task is retained for A->C but cancelled by a direct new
    // Move/Attack/Build command.
    uint64_t replacementExternalRevision = 0;
    // Provenance for the replacement identified above.  A synchronous Stop
    // leaves no ObjectOrderIntent in the queue, but autonomous state machines
    // still need the original Player/Script and command-family distinction.
    // These fields are meaningful only while replacementExternalRevision ==
    // externalRevision.
    ObjectOrderSource replacementExternalSource = ObjectOrderSource::System;
    ObjectOrderKind replacementExternalKind = ObjectOrderKind::Move;
};

} // namespace engine
