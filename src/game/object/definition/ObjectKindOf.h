#pragma once

#include "core/container/bit_flags.h"
#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace game {

// Stable Zero Hour KindOf ordinal. The order is identical to GeneralsMD's
// KindOfType with RTS_GENERALS enabled and ALLOW_SURRENDER disabled.
enum class ObjectKindOf : uint8_t {
    Obstacle,
    Selectable,
    Immobile,
    CanAttack,
    StickToTerrainSlope,
    CanCastReflections,
    Shrubbery,
    Structure,
    Infantry,
    Vehicle,
    Aircraft,
    HugeVehicle,
    Dozer,
    Harvester,
    CommandCenter,
    LineBuild,
    Salvager,
    WeaponSalvager,
    Transport,
    Bridge,
    LandmarkBridge,
    BridgeTower,
    Projectile,
    Preload,
    NoGarrison,
    Waveguide,
    WaveEffect,
    NoCollide,
    RepairPad,
    HealPad,
    StealthGarrison,
    CashGenerator,
    Airfield,
    DrawableOnly,
    MpCountForVictory,
    RebuildHole,
    Score,
    ScoreCreate,
    ScoreDestroy,
    NoHealIcon,
    CanRappel,
    Parachutable,
    CanBeRepulsed,
    MobNexus,
    IgnoredInGui,
    Crate,
    Capturable,
    ClearedByBuild,
    SmallMissile,
    AlwaysVisible,
    Unattackable,
    Mine,
    CleanupHazard,
    PortableStructure,
    AlwaysSelectable,
    AttackNeedsLineOfSight,
    WalkOnTopOfWall,
    DefensiveWall,
    FsPower,
    FsFactory,
    FsBaseDefense,
    FsTechnology,
    AircraftPathAround,
    LowOverlappable,
    ForceAttackable,
    AutoRallypoint,
    TechBuilding,
    Powered,
    ProducedAtHelipad,
    Drone,
    CanSeeThroughStructure,
    BallisticMissile,
    ClickThrough,
    SupplySourceOnPreview,
    Parachute,
    GarrisonableUntilDestroyed,
    Boat,
    ImmuneToCapture,
    Hulk,
    ShowPortraitWhenControlled,
    SpawnsAreTheWeapons,
    CannotBuildNearSupplies,
    SupplySource,
    RevealToAll,
    Disguiser,
    Inert,
    Hero,
    IgnoresSelectAll,
    DontAutoCrushInfantry,
    CliffJumper,
    FsSupplyDropzone,
    FsSuperweapon,
    FsBlackMarket,
    FsSupplyCenter,
    FsStrategyCenter,
    MoneyHacker,
    ArmorSalvager,
    RevealsEnemyPaths,
    BoobyTrap,
    FsFake,
    FsInternetCenter,
    BlastCrater,
    Prop,
    OptimizedTree,
    FsAdvancedTech,
    FsBarracks,
    FsWarfactory,
    FsAirfield,
    AircraftCarrier,
    NoSelect,
    RejectUnmanned,
    CannotRetaliate,
    TechBaseDefense,
    EmpHardened,
    Demotrap,
    ConservativeBuilding,
    IgnoreDockingBones,
    Count,
};

inline constexpr size_t kObjectKindOfCount =
    static_cast<size_t>(ObjectKindOf::Count);
using ObjectKindOfMask = container::BitFlags<kObjectKindOfCount>;

[[nodiscard]] constexpr size_t objectKindOfIndex(ObjectKindOf kind) noexcept {
    return static_cast<size_t>(kind);
}

[[nodiscard]] inline bool objectHasKind(const ObjectKindOfMask& mask,
                                        ObjectKindOf kind) noexcept {
    return mask.test(objectKindOfIndex(kind));
}

inline void setObjectKind(ObjectKindOfMask& mask, ObjectKindOf kind,
                          bool enabled = true) noexcept {
    mask.set(objectKindOfIndex(kind), enabled);
}

[[nodiscard]] inline bool objectKindsMatch(
    const ObjectKindOfMask& values, const ObjectKindOfMask& required,
    const ObjectKindOfMask& forbidden) noexcept {
    return values.test_for_all(required) && values.test_for_none(forbidden);
}

[[nodiscard]] std::optional<ObjectKindOf>
parseObjectKindOf(container::StringView name) noexcept;

[[nodiscard]] container::StringView
objectKindOfName(ObjectKindOf kind) noexcept;

// Compiles the final inherited KindOf text at the content boundary. Unknown
// tokens are returned to the caller for diagnostics and never enter ECS.
[[nodiscard]] bool compileObjectKindOfMask(
    container::StringView text, ObjectKindOfMask& output,
    container::Vector<container::String>* unknownTokens = nullptr);

} // namespace game
