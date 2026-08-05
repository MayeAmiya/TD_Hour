#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include "game/base/DamageTypes.h"
#include "game/base/ObjectVeterancy.h"
#include "game/object/definition/ObjectModuleCatalog.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

namespace game {

struct ThingTemplate;
struct ObjectFireWeaponWhenDeadParameters;

// A compact, game-wide vocabulary for Object status conditions.  It is a
// modern value mask rather than RefCode's mutable Object pointer state, but
// preserves the authored names consumed by DieMuxData.  We do not support
// legacy save binaries, so the bit layout is intentionally dense and local
// to the modern engine rather than inheriting old serialized padding.
enum class ObjectStatusFlag : uint8_t {
    Destroyed,
    CanAttack,
    UnderConstruction,
    Unselectable,
    NoCollisions,
    NoAttack,
    AirborneTarget,
    Parachuting,
    Repulsor,
    Hijacked,
    Aflame,
    Burned,
    Wet,
    IsFiringWeapon,
    IsBraking,
    Stealthed,
    Detected,
    CanStealth,
    Sold,
    UndergoingRepair,
    Reconstructing,
    Masked,
    IsAttacking,
    IsUsingAbility,
    IsAimingWeapon,
    NoAttackFromAi,
    IgnoringStealth,
    IsCarBomb,
    DeckHeightOffset,
    Rider1,
    Rider2,
    Rider3,
    Rider4,
    Rider5,
    Rider6,
    Rider7,
    Rider8,
    FaerieFire,
    MissileKillingSelf,
    ReassignParking,
    BoobyTrapped,
    Immobile,
    Disguised,
    Deployed,
    ScriptUnstealthed,
    Count,
};

using ObjectStatusMask = uint64_t;

[[nodiscard]] constexpr ObjectStatusMask objectStatusBit(ObjectStatusFlag status) noexcept {
    return ObjectStatusMask{1} << static_cast<uint8_t>(status);
}

// The modern runtime intentionally keeps the legacy vocabulary, but not its
// serialized bit layout.  Callers which receive a raw mask must clamp it to
// this set before mutating an entity, so an invalid content/API bit can never
// turn into an unobservable persistent state.
[[nodiscard]] constexpr ObjectStatusMask objectStatusKnownMask() noexcept
{
    static_assert(static_cast<uint8_t>(ObjectStatusFlag::Count) < 64);
    return (ObjectStatusMask{1} << static_cast<uint8_t>(ObjectStatusFlag::Count)) - ObjectStatusMask{1};
}

struct ObjectStatusMaskParseResult final
{
    ObjectStatusMask mask = 0;
    bool resolved = true;
};

// Shared parser for all ObjectStatusMaskType-style INI fields.  It preserves
// RefCode's ALL/NONE and +/- grammar while exposing failure explicitly instead
// of making every module family duplicate a string-to-bit table.
[[nodiscard]] ObjectStatusMaskParseResult parseObjectStatusMask(container::StringView value,
                                                                ObjectStatusMask initialMask = 0);

// These are the DieModule classes with a typed ECS reaction in this stage.
// Unsupported classes are still frozen in the plan so content coverage
// remains observable and later migration does not need to reparse module
// recipes from a mutable store.
enum class ObjectDeathReactionKind : uint8_t {
    Destroy,
    KeepObject,
    // FXListDie is a non-terminal Die callback.  It emits a detached
    // presentation command and deliberately lets later authored Die rules
    // (normally DestroyDie) continue to run in the same transaction.
    FxList,
    // UpgradeDie removes one OBJECT upgrade from the stable producer recorded
    // on the dying object. It is non-terminal and later Die callbacks continue
    // in author order.
    Upgrade,
    // CrushDie is non-terminal: it records the selected front/back crush
    // state and model conditions, then later authored Die rules continue.
    Crush,
    // FireWeaponWhenDeadBehavior is a non-terminal Die callback that emits a
    // system-owned temporary weapon command at the object's current position.
    FireWeaponWhenDead,
    // LeafletDropBehavior applies its radius disable as a non-terminal Die
    // callback, then the authored death walk continues.
    LeafletDrop,
    // Chooses the authored air/ground ObjectCreationList from the current
    // flight state, publishes the two per-unit eject sounds, and continues
    // the DieMux walk. RefCode parses InvulnerableTime but does not consume it.
    EjectPilot,
    // Enables every WAVEGUIDE by clearing only DISABLED_DEFAULT. This is a
    // non-terminal map-wide Die callback used by the stock destructible dam.
    Dam,
    // Evaluates one or more named CrateData recipes and emits a structural
    // crate-spawn intent. Selection remains a confirmed random transaction.
    CreateCrate,
    // Creates an ObjectCreationList at the dead object's transform. The OCL
    // executor reports its first created object so the legacy previous-health
    // transfer can complete without retaining an Object pointer.
    CreateObject,
    // Notifies the script boundary that the creator associated with this
    // transient special-power object has completed its authored power.  The
    // creator binding is sparse per-object runtime state because weapon/OCL
    // creation may explicitly lock it to INVALID to suppress duplicates.
    SpecialPowerCompletion,
    // NeutronBlastBehavior is an ordinary non-terminal Die callback.  Its
    // radius scan therefore runs only when Body dispatches onDie, never merely
    // because a projectile reached its destination or was structurally
    // deleted.
    NeutronBlast,
    // Terminal building motion is both an Update and a Die interface in the
    // legacy recipe.  The modern death walk starts a sparse fixed-tick state
    // machine and then continues to later authored Die callbacks.
    StructureTopple,
    StructureCollapse,
    // Jet/Helicopter slow death is a real authored Die callback. It starts
    // one matching Airfield runtime at this exact DieMux position instead of
    // polling effectivelyDead later from the generic AirOperations phase.
    AircraftSlowDeath,
    RebuildHoleExpose,
    RebuildHoleBehavior,
    InstantDeath,
    SlowDeath,
    Unsupported,
    Count,
};

struct ObjectOnDieBehaviorEntry final {
    ObjectOnDieHandlerKind handler = ObjectOnDieHandlerKind::Unknown;
    uint32_t authoredOrder = 0;
    uint32_t finalBehaviorIndex = 0;
    uint32_t reactionRuleIndex = std::numeric_limits<uint32_t>::max();
    uint32_t deathTypeMask = 0;
    ObjectVeterancyMask veterancyMask = 0;
    ObjectStatusMask exemptStatuses = 0;
    ObjectStatusMask requiredStatuses = 0;
    bool filtersFullyResolved = true;
};

enum class ObjectSlowDeathPhase : uint8_t {
    Initial,
    Midpoint,
    Final,
    Count,
};

// Immutable typed projection of the generic SlowDeathBehavior fields. Phase
// payloads are *names only*: ObjectSimulation publishes them as value events
// and neither spawns an object nor touches a W3D/FX resource. That keeps the
// confirmed death schedule independent of renderer/resource lifetime.
struct ObjectSlowDeathParameters final {
    int32_t probabilityModifier = 10;
    math::q32_32 modifierBonusPerOverkillPercent{};
    uint32_t sinkDelayMilliseconds = 0;
    uint32_t sinkDelayVarianceMilliseconds = 0;
    uint32_t destructionDelayMilliseconds = 0;
    uint32_t destructionDelayVarianceMilliseconds = 0;
    math::q32_32 sinkRateUnitsPerSecond{};
    // SlowDeathBehaviorModuleData parses and serializes this legacy field,
    // but RefCode never reads it in beginSlowDeath() or update(). The compiler
    // still validates the authored value without retaining dead runtime state.
    // Authored values are legacy per-frame forces. The plan compiler converts
    // them once to mass*world-units/second^2 so confirmed ticks can feed the
    // canonical fixed-point physics integrator without float or unit repair.
    math::q32_32 flingForce{};
    math::q32_32 flingForceVariance{};
    math::q32_32 flingPitchRadians{};
    math::q32_32 flingPitchVarianceRadians{};
    // BattleBusSlowDeathBehavior::onDie marks the module as a real death and
    // cancels its earlier undeath/landed continuation before delegating to
    // the common SlowDeathBehavior implementation.
    bool cancelsBattleBusUndeath = false;
    container::Array<container::Vector<container::String>, static_cast<size_t>(ObjectSlowDeathPhase::Count)> fx;
    container::Array<container::Vector<container::String>, static_cast<size_t>(ObjectSlowDeathPhase::Count)> ocls;
    container::Array<container::Vector<container::String>, static_cast<size_t>(ObjectSlowDeathPhase::Count)> weapons;
};

// Immutable projection of InstantDeathBehavior's three independently
// selected payload lists.  RefCode parses every token from every repeated
// FX/OCL/Weapon field into a flat list, rather than using SlowDeath's phase
// prefix grammar.  Keep that distinction in the typed recipe so an ordinary
// InstantDeath payload can never be misread as a phase name.
struct ObjectInstantDeathParameters final {
    container::Vector<container::String> fx;
    container::Vector<container::String> ocls;
    container::Vector<container::String> weapons;
};

struct ObjectCreateObjectDieParameters final {
    container::String creationList;
    bool transferPreviousHealth = false;
    bool transferSelection = false;
};

struct ObjectSpecialPowerCompletionDieParameters final {
    container::String specialPowerTemplate;
};

struct ObjectNeutronBlastDieParameters final {
    math::q32_32 blastRadius{10.0f};
    bool affectAirborne = true;
    bool affectAllies = true;
};

struct ObjectEjectPilotDieParameters final {
    container::String airCreationList;
    container::String groundCreationList;
    uint32_t invulnerableTimeMilliseconds = 0;
};

struct ObjectCreateCrateDieParameters final {
    container::Vector<container::String> crateData;
};

enum class ObjectStructureTopplePhase : uint8_t {
    Initial,
    Delay,
    Final,
    Count,
};

enum class ObjectStructureCollapsePhase : uint8_t {
    Initial,
    Delay,
    Burst,
    Final,
    Count,
};

struct ObjectStructureAngleFx final {
    math::q32_32 angleRadians{};
    container::String fx;
};

struct ObjectStructureToppleParameters final {
    uint32_t minToppleDelayMilliseconds = 0;
    uint32_t maxToppleDelayMilliseconds = 0;
    uint32_t minBurstDelayMilliseconds = 0;
    uint32_t maxBurstDelayMilliseconds = 0;
    math::q32_32 structuralIntegrity{0.1};
    math::q32_32 structuralDecay{};
    uint64_t damageFxTypes = std::numeric_limits<uint64_t>::max();
    container::String topplingFx; // retained: RefCode parses but never emits it
    container::String toppleStartFx;
    container::String toppleDelayFx;
    container::String toppleDoneFx;
    container::String crushingFx;
    container::String crushingWeapon;
    container::Array<container::Vector<container::String>,
                     static_cast<size_t>(ObjectStructureTopplePhase::Count)>
        ocls;
    container::Vector<ObjectStructureAngleFx> angleFx;
};

struct ObjectStructureCollapseParameters final {
    uint32_t minCollapseDelayMilliseconds = 0;
    uint32_t maxCollapseDelayMilliseconds = 0;
    uint32_t minBurstDelayMilliseconds = 9999;
    uint32_t maxBurstDelayMilliseconds = 9999;
    math::q32_32 collapseDamping{};
    math::q32_32 maxShudder{};
    uint32_t bigBurstFrequency = 0;
    container::Array<container::Vector<container::String>,
                     static_cast<size_t>(ObjectStructureCollapsePhase::Count)>
        fx;
    container::Array<container::Vector<container::String>,
                     static_cast<size_t>(ObjectStructureCollapsePhase::Count)>
        ocls;
};

// Immutable projection of FXListDie.  The legacy class is both a Die module
// and an UpgradeMux: its own activation is sticky, while conflicts are
// tested again when the object dies.  We retain the names rather than old
// UpgradeMask bits because the modern player registry owns canonical upgrade
// strings until the global Upgrade catalog is migrated to frozen IDs.
struct ObjectFxListDieParameters final
{
    container::String deathFx;
    container::Vector<container::String> triggeredBy;
    container::Vector<container::String> conflictsWith;
    container::Vector<container::String> removesUpgrades;
    engine::UpgradeMask triggeredByMask;
    engine::UpgradeMask conflictsWithMask;
    engine::UpgradeMask removesUpgradesMask;
    container::String upgradeFx;
    bool orientToObject = true;
    // RefCode defaults this to true despite the historical comment claiming
    // otherwise; most stock FXListDie declarations rely on that default.
    bool startsActive = true;
    bool requiresAllTriggers = false;
    bool upgradeMasksCompiled = false;
};

struct ObjectUpgradeDieParameters final {
    container::String upgradeToRemove;
    engine::UpgradeContentId upgradeToRemoveId =
        engine::INVALID_UPGRADE_CONTENT_ID;
};

enum class ObjectCrushType : uint8_t {
    Total,
    BackEnd,
    FrontEnd,
    None,
};

struct ObjectCrushDieParameters final {
    container::Array<container::String, 3> sounds;
    container::Array<int32_t, 3> soundPercents{100, 100, 100};
};

struct ObjectDeathReactionRule final {
    ObjectDeathReactionKind kind = ObjectDeathReactionKind::Unsupported;
    uint32_t authoredOrder = 0;
    uint32_t deathTypeMask = 0;
    ObjectVeterancyMask veterancyMask = 0;
    ObjectStatusMask exemptStatuses = 0;
    ObjectStatusMask requiredStatuses = 0;
    // Malformed/unknown legacy tokens must never be treated as an arbitrary
    // matching action. The content parser retains the original ModuleData;
    // this plan simply makes the runtime fallback explicit and safe.
    bool filtersFullyResolved = true;
    // Present for generic SlowDeathBehavior and specialized variants whose
    // dedicated ECS controllers compose the common destruction/OCL/FX clock.
    std::optional<ObjectSlowDeathParameters> slowDeath;
    // Present only for InstantDeathBehavior. The selected values cross the
    // confirmed-frame boundary as detached commands; an OCL/object factory
    // and temporary-weapon executor deliberately remain separate systems.
    std::optional<ObjectInstantDeathParameters> instantDeath;
    std::optional<ObjectCreateObjectDieParameters> createObjectDie;
    std::optional<ObjectSpecialPowerCompletionDieParameters>
        specialPowerCompletionDie;
    std::optional<ObjectNeutronBlastDieParameters> neutronBlastDie;
    std::optional<ObjectEjectPilotDieParameters> ejectPilotDie;
    std::optional<ObjectCreateCrateDieParameters> createCrateDie;
    std::optional<ObjectStructureToppleParameters> structureTopple;
    std::optional<ObjectStructureCollapseParameters> structureCollapse;
    // Present only for FXListDie.  Its activation state is deliberately
    // mutable per entity and lives in ObjectFxListDieRuntimeComponent.
    std::optional<ObjectFxListDieParameters> fxListDie;
    // Present only for UpgradeDie. The producer relation remains per entity
    // in ObjectProducedByComponent; the immutable rule stores only content.
    std::optional<ObjectUpgradeDieParameters> upgradeDie;
    // Present only for CrushDie. Body front/back state is per entity.
    std::optional<ObjectCrushDieParameters> crushDie;
    // Present only for FireWeaponWhenDeadBehavior. Activation/cooldown state
    // is per entity; the immutable payload owns names and UpgradeMux data.
    container::SharedPtr<const ObjectFireWeaponWhenDeadParameters>
        fireWeaponWhenDead;
};

struct ObjectDeathReactionPlan final {
    // Full final Behavior-order Die interface walk. This is the authoritative
    // sequence for the new DeathWalk executor; `rules` remains the typed
    // payload pool used by DeathReaction entries during migration.
    container::Vector<ObjectOnDieBehaviorEntry> onDieBehaviors;
    container::Vector<ObjectDeathReactionRule> rules;
    container::Vector<container::String> diagnostics;
    uint32_t unknownOnDieHandlerCount = 0;
    // The source InstantDeathBehavior and SlowDeathBehavior use
    // AIUpdateInterface::isAiInDeadState() as a one-death-reaction latch.
    // Recipe compilation already identifies stock AI modules, so retain this
    // capability bit without recreating a virtual AI object. Objects with no
    // AI module preserve RefCode's ungated behavior.
    bool hasAiDeathGate = false;
};

// Compiles the effective, inheritance-resolved Object recipe into immutable
// value data. A null result means the template has no Die interface at all;
// final content must not invent an implicit DestroyDie in that case.
[[nodiscard]] container::SharedPtr<const ObjectDeathReactionPlan>
compileObjectDeathReactionPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

[[nodiscard]] bool isObjectDeathReactionApplicable(
    const ObjectDeathReactionRule& rule, DeathType deathType,
    ObjectVeterancyLevel veterancy, ObjectStatusMask statuses) noexcept;

[[nodiscard]] bool isObjectOnDieBehaviorApplicable(
    const ObjectOnDieBehaviorEntry& entry, DeathType deathType,
    ObjectVeterancyLevel veterancy, ObjectStatusMask statuses) noexcept;

// UpgradeMux predicates used by the per-object FXListDie runtime.  The
// completed list is PlayerRegistry's canonical, case-insensitively sorted
// representation.  Keeping these pure makes spawn and upgrade-completion
// paths use exactly the same activation/conflict semantics.
[[nodiscard]] bool objectFxListDieUpgradeTriggersSatisfied(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;
[[nodiscard]] bool objectFxListDieUpgradeTriggersSatisfied(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;
[[nodiscard]] bool objectFxListDieHasUpgradeConflict(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;
[[nodiscard]] bool objectFxListDieHasUpgradeConflict(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;

} // namespace game

namespace engine {

// A status mutation can affect more than the flag word.  These dependency
// bits are a pull-friendly invalidation contract for the modern ECS: a
// future shroud, collision, selection or stealth system can consume the
// revision without ObjectStatus owning legacy Object*/Partition pointers.
enum class ObjectStatusDependency : uint8_t
{
    Spatial,
    Visibility,
    Collision,
    Combat,
    Selection,
    Construction,
    Repulsor,
    Stealth,
    Lifecycle,
    Count,
};

using ObjectStatusDependencyMask = uint32_t;

[[nodiscard]] constexpr ObjectStatusDependencyMask objectStatusDependencyBit(ObjectStatusDependency dependency) noexcept
{
    return dependency >= ObjectStatusDependency::Count
               ? ObjectStatusDependencyMask{}
               : static_cast<ObjectStatusDependencyMask>(ObjectStatusDependencyMask{1}
                                                         << static_cast<uint8_t>(dependency));
}

// Runtime status/veterancy are intentionally independent of immutable
// content. They can be added by their dedicated systems later without making
// a Die reaction retain an Object pointer or parse strings during a tick.
struct ObjectStatusComponent final {
    game::ObjectStatusMask flags = 0;
    // A monotonic value-level revision lets consumers efficiently observe a
    // real transition.  It advances only when the sanitized final mask
    // differs; repeated set/clear requests are intentionally inert.
    uint64_t revision = 0;
    uint64_t lastChangedTick = 0;
    game::ObjectStatusMask lastSetMask = 0;
    game::ObjectStatusMask lastClearedMask = 0;
    ObjectStatusDependencyMask pendingDependencies = 0;
    // ZH Object::m_singleUseCommandUsed is object-wide rather than tied to a
    // particular button.  Once an accepted SINGLE_USE_COMMAND marks this
    // value, every ControlBar command for the object remains restricted.
    // Command routing owns the false->true transition; availability is a
    // read-only projection of this durable simulation fact.
    bool singleUseCommandUsed = false;

    [[nodiscard]] bool hasAny(game::ObjectStatusMask mask) const noexcept {
        return (flags & mask) != 0;
    }
    [[nodiscard]] bool hasAll(game::ObjectStatusMask mask) const noexcept {
        return (flags & mask) == mask;
    }
};

struct ObjectVeterancyComponent final {
    game::ObjectVeterancyLevel level = game::ObjectVeterancyLevel::Regular;
};

// Each entity only holds the shared immutable profile owned by its frozen
// ObjectArchetype. A null plan is meaningful: it records a profiled final
// recipe with no Die interface, which must not silently become DestroyDie.
// This avoids copying strings, module data, or a vector of reaction rules
// into every tank/building instance.
struct ObjectDeathReactionComponent final {
    container::SharedPtr<const game::ObjectDeathReactionPlan> plan;
};

struct ObjectCrushStateComponent final {
    bool frontCrushed = false;
    bool backCrushed = false;
};

// Per-object mutable counterpart to the immutable FXListDie rules.  Entries
// are indexed by ObjectDeathReactionPlan::rules, not by an EnTT iteration
// order.  This mirrors UpgradeMux's one-way `m_upgradeExecuted` state while
// letting a later player upgrade suppress a previously active default effect
// through its conflict mask.
struct ObjectFxListDieRuleRuntime final
{
    bool activated = false;
    bool playerConflict = false;
};

struct ObjectFxListDieRuntimeComponent final
{
    container::Vector<ObjectFxListDieRuleRuntime> rules;
};

struct ObjectSpecialPowerCompletionRuleRuntime final {
    ObjectId creator = INVALID_OBJECT_ID;
    bool creatorSet = false;
};

// SpecialPowerCompletionDie::setCreator is a first-write-wins operation on
// the first matching Die module.  Keep a slot per authored rule so multiple
// modules remain representable without storing a virtual module pointer.
struct ObjectSpecialPowerCompletionRuntimeComponent final {
    container::Vector<ObjectSpecialPowerCompletionRuleRuntime> rules;
};

// Mutable schedule for one selected generic SlowDeath rule. An object can
// enter it once because ObjectHealthComponent::terminalDeathIssued gates
// duplicate Body death transactions. All times are absolute confirmed ticks;
// no renderer delta or wall clock is allowed to decide destruction.
struct ObjectSlowDeathRuntimeComponent final {
    uint32_t selectedRuleIndex = 0;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    uint64_t randomKey = 0;
    uint64_t deathTick = 0;
    uint64_t sinkTick = 0;
    uint64_t midpointTick = 0;
    uint64_t destructionTick = 0;
    uint64_t lastMotionTick = 0;
    bool initialEmitted = false;
    bool midpointEmitted = false;
    bool finalEmitted = false;
    bool motionTickInitialized = false;
    bool flungIntoAir = false;
    bool minimumFlingAltitudeApplied = false;
    bool flingLanded = false;
    bool snaggedInShrubbery = false;
};

} // namespace engine
