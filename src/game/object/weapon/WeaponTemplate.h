#pragma once

#include "core/container/hash_containers.h"
#include "game/data/base/LegacyIniLoadType.h"

#include "game/object/creation/ObjectCreationListCatalog.h"

#include "game/base/DamageTypes.h"
#include "math/fixed/q32_32.h"
#include <compare>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
namespace game {

// Stable only inside one GameContentSnapshot.  It deliberately is not a
// global WeaponStore pointer or a serialized cross-match identity: the
// session content fingerprint protects the latter boundary, while the ID
// gives hot ECS weapon queries a compact O(1) immutable lookup.
struct WeaponContentId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const WeaponContentId&) const noexcept = default;
};

inline constexpr WeaponContentId INVALID_WEAPON_CONTENT_ID{};
inline constexpr float kLegacyInstantaneousWeaponSpeed = 999999.0f;

// Weapon bonuses are an independent condition domain.  In particular,
// PlayerUpgrade here is not WeaponSetCondition::PlayerUpgrade: the former
// changes scalar weapon behaviour while the latter selects another authored
// WeaponSet.  Values intentionally retain the original stable bit indices so
// projectile inheritance and future veterancy/horde systems can exchange one
// compact value mask without string lookups in confirmed simulation.
enum class WeaponBonusCondition : uint8_t {
    Garrisoned = 0,
    Horde,
    ContinuousFireMean,
    ContinuousFireFast,
    Nationalism,
    PlayerUpgrade,
    DroneSpotting,
    Demoralized,
    Enthusiastic,
    Veteran,
    Elite,
    Hero,
    BattleplanBombardment,
    BattleplanHoldTheLine,
    BattleplanSearchAndDestroy,
    Subliminal,
    SoloHumanEasy,
    SoloHumanNormal,
    SoloHumanHard,
    SoloAiEasy,
    SoloAiNormal,
    SoloAiHard,
    TargetFaerieFire,
    Fanaticism,
    FrenzyOne,
    FrenzyTwo,
    FrenzyThree,
    Count,
};

using WeaponBonusConditionMask = uint32_t;

[[nodiscard]] constexpr WeaponBonusConditionMask
weaponBonusConditionBit(WeaponBonusCondition condition) noexcept {
    return condition >= WeaponBonusCondition::Count
        ? WeaponBonusConditionMask{}
        : WeaponBonusConditionMask{1} << static_cast<uint8_t>(condition);
}

enum class WeaponBonusField : uint8_t {
    Damage = 0,
    Radius,
    Range,
    RateOfFire,
    PreAttack,
    Count,
};

inline constexpr size_t kWeaponBonusConditionCount =
    static_cast<size_t>(WeaponBonusCondition::Count);
inline constexpr size_t kWeaponBonusFieldCount =
    static_cast<size_t>(WeaponBonusField::Count);

[[nodiscard]] std::optional<WeaponBonusCondition>
parseWeaponBonusCondition(container::StringView value) noexcept;
[[nodiscard]] std::optional<WeaponBonusField>
parseWeaponBonusField(container::StringView value) noexcept;

// Authoritative bonus values use Q32.32.  A WeaponBonus begins at identity,
// and append() reproduces RefCode's deliberately additive composition:
//   result = 1 + sum(each active multiplier - 1)
// It is not multiplicative (125% + 125% therefore resolves to 150%).
struct WeaponBonus final {
    using Scalar = math::q32_32;

    container::Array<Scalar, kWeaponBonusFieldCount> multipliers{
        Scalar{int32_t{1}}, Scalar{int32_t{1}}, Scalar{int32_t{1}},
        Scalar{int32_t{1}}, Scalar{int32_t{1}},
    };

    [[nodiscard]] Scalar multiplier(WeaponBonusField field) const noexcept;
    // Applies one resolved field without exposing q32_32's wrapping scalar
    // operators to gameplay callers. Extreme mod data saturates at the
    // destination representation instead of becoming negative or trapping.
    [[nodiscard]] Scalar scale(Scalar value, WeaponBonusField field) const noexcept;
    void set(WeaponBonusField field, Scalar value) noexcept;
    void append(const WeaponBonus& other) noexcept;
};

struct WeaponBonusSet final {
    container::Array<WeaponBonus, kWeaponBonusConditionCount> conditions;

    void set(WeaponBonusCondition condition, WeaponBonusField field,
             WeaponBonus::Scalar value) noexcept;
    void append(WeaponBonusConditionMask activeConditions,
                WeaponBonus& destination) const noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Reads repeated `WeaponBonus = CONDITION FIELD 125%` entries from every
    // GameData layer in authoring order.  A later entry replaces only the same
    // condition/field pair, matching the legacy FieldParse overlay behaviour.
    [[nodiscard]] static bool loadFromLegacyGameData(container::StringView path,
                                                      WeaponBonusSet& output,
                                                      container::String* error = nullptr);
};

enum class WeaponReloadType : uint8_t {
    Auto,
    None,
    ReturnToBase,
};

enum class WeaponPreAttackType : uint8_t {
    PerShot,
    PerAttack,
    PerClip,
};

// Typed result carried by confirmed weapon/projectile events. Presentation
// consumes this policy without re-deriving mutable stealth or timing state.
enum class WeaponFxPolicy : uint8_t {
    Play,
    SuppressedBySuspendDelay,
    SuppressedByStealth,
};

// Typed copies of the original Weapon anti-target categories.  Keeping them
// as masks makes target validation data-oriented and leaves room for future
// projectile/airborne components without string comparisons in the hot path.
enum class WeaponAntiTarget : uint8_t {
    AirborneVehicle = 0,
    Ground,
    Projectile,
    SmallMissile,
    Mine,
    AirborneInfantry,
    BallisticMissile,
    Parachute,
};

using WeaponAntiMask = uint32_t;

[[nodiscard]] constexpr WeaponAntiMask weaponAntiBit(WeaponAntiTarget value) noexcept {
    return WeaponAntiMask{1} << static_cast<uint8_t>(value);
}

enum class WeaponAffectsTarget : uint8_t {
    Self = 0,
    Allies,
    Enemies,
    Neutrals,
    KillsSelf,
    NotSimilar,
    NotAirborne,
};

using WeaponAffectsMask = uint32_t;

[[nodiscard]] constexpr WeaponAffectsMask weaponAffectsBit(WeaponAffectsTarget value) noexcept {
    return WeaponAffectsMask{1} << static_cast<uint8_t>(value);
}

enum class WeaponCollideTarget : uint8_t {
    Allies = 0,
    Enemies,
    Structures,
    Shrubbery,
    Projectiles,
    Walls,
    SmallMissiles,
    BallisticMissiles,
    ControlledStructures,
};

using WeaponCollideMask = uint32_t;

[[nodiscard]] constexpr WeaponCollideMask weaponCollideBit(WeaponCollideTarget value) noexcept {
    return WeaponCollideMask{1} << static_cast<uint8_t>(value);
}

// Immutable presentation recipe compiled from Weapon::ProjectileStreamName
// and the referenced W3DProjectileStreamDraw object.  Gameplay retains only
// the stream object name; this descriptor is resolved when a session freezes
// its content so the renderer never parses Thing recipes or mutable INI data.
enum class ProjectileStreamBlendMode : uint8_t {
    Additive,
    Alpha,
    Multiply,
    Opaque,
};

enum class ProjectileStreamDepthMode : uint8_t {
    TestNoWrite,
    TestWrite,
    Disabled,
};

struct ProjectileStreamRenderDescriptor final {
    container::String texture;
    container::Array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    float width = 0.0f;
    float tileFactor = 1.0f;
    float scrollRate = 0.0f;
    // Stock W3D streams have no authored lingering time and therefore use
    // zero. Modern content may opt into a confirmed-frame fade after a
    // projectile disappears without making render delta authoritative.
    float segmentLifetimeSeconds = 0.0f;
    uint32_t maximumSegments = 0;
    ProjectileStreamBlendMode blend = ProjectileStreamBlendMode::Additive;
    ProjectileStreamDepthMode depth = ProjectileStreamDepthMode::TestNoWrite;
    bool enabled = false;
};

// Canonical confirmed-simulation copy of every authored real-valued Weapon
// field. The parallel float members below remain the permissive INI and
// presentation compatibility surface; gameplay must consume this Q32.32
// block so a frozen template is quantized exactly once.
struct WeaponAuthoritativeScalars final {
    using Scalar = math::q32_32;

    Scalar primaryDamage{};
    Scalar primaryDamageRadius{};
    Scalar secondaryDamage{};
    Scalar secondaryDamageRadius{};
    Scalar attackRange{};
    Scalar minimumAttackRange{};
    Scalar requestAssistRange{};
    Scalar acceptableAimDeltaRadians{};
    Scalar minTargetPitchRadians{-std::numbers::pi_v<float>};
    Scalar maxTargetPitchRadians{std::numbers::pi_v<float>};
    Scalar radiusDamageAngleRadians{std::numbers::pi_v<float>};
    Scalar scatterRadius{};
    Scalar scatterTargetScalar{};
    Scalar scatterRadiusVsInfantry{};
    Scalar continueAttackRange{};
    Scalar weaponSpeed{kLegacyInstantaneousWeaponSpeed};
    Scalar minimumWeaponSpeed{kLegacyInstantaneousWeaponSpeed};
    Scalar weaponRecoilRadians{};
    Scalar stunDuration{};
    // Canonical force in mass*world-units/second^2. The authoring mirror below
    // retains the legacy per-frame value until synchronizeAuthoritativeScalars.
    Scalar shockWaveAmount{};
    Scalar shockWaveRadius{};
    Scalar shockWaveTaperOff{};
    Scalar historicBonusRadius{};
};

struct WeaponScatterTarget final {
    math::q32_32 x{};
    math::q32_32 y{};
};

// Immutable session/runtime database record. It intentionally contains no
// authoring float mirrors; every real-valued gameplay field is available only
// through `fixed` after Weapon.ini and map/mod overrides have been merged.
struct WeaponTemplate {
    container::String name;
    DamageType damageType = DamageType::EXPLOSION;
    // Single authored ObjectStatus bit used by DAMAGE_STATUS. Zero means the
    // weapon has no valid status payload and the Body transaction is inert.
    uint64_t damageStatusMask = 0;
    DeathType deathType = DeathType::NORMAL;
    bool scaleWeaponSpeed = false;
    // Consecutive shots retained on one visual barrel before advancing to
    // the next Name01..99 launch family. RefCode defaults this to one; the
    // frozen modern form normalizes non-positive authoring to one.
    uint32_t shotsPerBarrel = 1;
    int32_t clipSize = 0;
    // RefCode WeaponTemplate::m_isShowsAmmoPips. ClipSize alone controls
    // reload gameplay and must not manufacture UI pips.
    bool showsAmmoPips = false;
    uint32_t clipReloadTimeMilliseconds = 0;
    uint32_t minimumDelayBetweenShotsMilliseconds = 0;
    uint32_t maximumDelayBetweenShotsMilliseconds = 0;
    // Object-level FiringTracker authoring. UINT32_MAX keeps ordinary
    // weapons out of the continuous-fire state machine unless explicitly
    // authored, matching WeaponTemplate's legacy INT_MAX defaults.
    uint32_t continuousFireOneShotsNeeded = UINT32_MAX;
    uint32_t continuousFireTwoShotsNeeded = UINT32_MAX;
    uint32_t continuousFireCoastMilliseconds = 0;
    uint32_t autoReloadWhenIdleMilliseconds = 0;
    uint32_t preAttackDelayMilliseconds = 0;
    WeaponReloadType reloadType = WeaponReloadType::Auto;
    WeaponPreAttackType preAttackType = WeaponPreAttackType::PerShot;
    uint32_t historicBonusTimeMilliseconds = 0;
    uint32_t historicBonusCount = 0;
    container::String historicBonusWeaponName;
    WeaponContentId historicBonusWeapon = INVALID_WEAPON_CONTENT_ID;
    container::Vector<WeaponScatterTarget> scatterTargets;
    bool leechRangeWeapon = false;
    container::String projectileObject;
    // Instant-hit weapons may create a W3DLaserDraw object whose LaserUpdate
    // follows this source bone and the admitted target. These are presentation
    // identities, but they must remain in the immutable weapon value so a
    // confirmed fire event can enter the lossless FX stream.
    container::String laserName;
    container::String laserBoneName;
    // RefCode MissileCallsOnDie: MissileAI applies its warhead first, then
    // immediately damages its own Body so authored Die modules run before
    // the delayed trail-retirement state.
    bool missileCallsOnDie = false;
    // ScriptActions::doNamedFireWeaponFollowingWaypointPath only admits a
    // Weapon instance carrying this authored capability.  It is selection
    // metadata, not a generic projectile-homing toggle.
    bool capableOfFollowingWaypoints = false;
    static constexpr size_t kVeterancyLevelCount = 4;
    container::Array<container::String, kVeterancyLevelCount> projectileExhausts;
    container::String projectileStreamName;
    ProjectileStreamRenderDescriptor projectileStream;
    container::String fireSound;
    uint32_t fireSoundLoopTimeMilliseconds = 0;
    // RefCode WeaponTemplate presentation policy. The delay is authored as a
    // duration, then converted once to a confirmed-tick deadline whenever a
    // concrete Weapon runtime is created or a WeaponSet recreates its slots.
    bool playFxWhenStealthed = false;
    uint32_t suspendFxDelayMilliseconds = 0;
    // ZH resolves both fire and detonation FX at the shooter's frozen
    // veterancy level. Base fields initialize all levels and authored
    // Veterancy* entries override one slot.
    container::Array<container::String, kVeterancyLevelCount> fireFXs;
    // Frozen even before the ObjectCreationList executor is migrated. This
    // prevents behavior/death weapons from silently losing their authored
    // poison-field, radiation-field or debris creation dependency.
    container::String fireOcl;
    // RefCode stores one FireOCL and ProjectileDetonationOCL per veterancy
    // level. Keep authored names in the reloadable store and resolve the
    // parallel IDs only in GameContentSnapshot.
    container::Array<container::String, kVeterancyLevelCount> fireOcls;
    container::Array<container::String, kVeterancyLevelCount> projectileDetonationOcls;
    container::Array<ObjectCreationListContentId, kVeterancyLevelCount> fireOclIds;
    container::Array<ObjectCreationListContentId, kVeterancyLevelCount>
        projectileDetonationOclIds;
    container::Array<container::String, kVeterancyLevelCount>
        projectileDetonationFXs;
    // Direct/area target semantics are typed at load time.  A later
    // projectile system uses the collide mask; the direct-hit ECS slice
    // already uses range/pitch and the AreaDamage system uses affects.
    WeaponAntiMask antiMask = weaponAntiBit(WeaponAntiTarget::Ground);
    WeaponAffectsMask radiusDamageAffects =
        weaponAffectsBit(WeaponAffectsTarget::Allies) |
        weaponAffectsBit(WeaponAffectsTarget::Enemies) |
        weaponAffectsBit(WeaponAffectsTarget::Neutrals);
    WeaponCollideMask projectileCollidesWith =
        weaponCollideBit(WeaponCollideTarget::Structures);
    bool damageDealtAtSelfPosition = false;
    // RefCode WeaponTemplate::m_allowAttackGarrisonedBldgs. It only widens the
    // damage *estimate* used for target admission: an occupied, clearable
    // garrison reports a small nonzero value even when the structure's armor
    // zeroes this damage type.
    bool allowAttackGarrisonedBldgs = false;
    WeaponAuthoritativeScalars fixed;
    // Per-template bonuses are appended after the frozen global GameData set.
    // Keeping the full small value table inline makes WeaponTemplate copies
    // self-contained across VFS overrides and GameContentSnapshot capture.
    WeaponBonusSet weaponBonuses;
};

// Reloadable authoring record. Float values exist only while parsing and
// applying legacy overwrite/CreateOverrides streams. GameContentSnapshot
// slices this type to WeaponTemplate after synchronizing `fixed`.
struct WeaponAuthoringTemplate final : WeaponTemplate {
    float primaryDamage = 0.0f;
    float primaryDamageRadius = 0.0f;
    float secondaryDamage = 0.0f;
    float secondaryDamageRadius = 0.0f;
    float attackRange = 0.0f;
    float minimumAttackRange = 0.0f;
    float requestAssistRange = 0.0f;
    float acceptableAimDeltaRadians = 0.0f;
    float minTargetPitchRadians = -std::numbers::pi_v<float>;
    float maxTargetPitchRadians = std::numbers::pi_v<float>;
    float radiusDamageAngleRadians = std::numbers::pi_v<float>;
    float scatterRadius = 0.0f;
    float scatterTargetScalar = 0.0f;
    float scatterRadiusVsInfantry = 0.0f;
    float continueAttackRange = 0.0f;
    float weaponSpeed = kLegacyInstantaneousWeaponSpeed;
    float minimumWeaponSpeed = kLegacyInstantaneousWeaponSpeed;
    float weaponRecoilRadians = 0.0f;
    float stunDuration = 0.0f;
    float shockWaveAmount = 0.0f;
    float shockWaveRadius = 0.0f;
    float shockWaveTaperOff = 0.0f;
    float historicBonusRadius = 0.0f;
    bool loaded = false;

    // Called after an INI definition/override has completed and again when
    // session content freezes. It is intentionally not used by frame logic.
    void synchronizeAuthoritativeScalars();
};

// Compatibility derivation for stateless transient weapon commands. Stateful
// Weapon instances use selectAndAdvanceWeaponBarrel instead.
[[nodiscard]] constexpr uint32_t weaponBarrelSequenceOrdinal(
    uint32_t weaponLocalShotSequence, uint32_t shotsPerBarrel) noexcept {
    if (weaponLocalShotSequence == 0) return 0;
    const uint32_t groupSize = shotsPerBarrel == 0 ? 1u : shotsPerBarrel;
    return ((weaponLocalShotSequence - 1u) / groupSize) + 1u;
}

// RefCode stores an explicit zero-based current barrel and a remaining-shot
// counter on each Weapon instance. The current visual state's first Draw
// module with a non-empty barrel table supplies barrelCount; a missing table
// behaves as one root-position barrel for deterministic modern fallback.
[[nodiscard]] constexpr uint32_t selectAndAdvanceWeaponBarrel(
    uint32_t& currentBarrel, uint32_t& shotsRemainingForCurrentBarrel,
    uint32_t barrelCount, uint32_t shotsPerBarrel) noexcept {
    const uint32_t normalizedCount = barrelCount == 0 ? 1u : barrelCount;
    const uint32_t normalizedShots = shotsPerBarrel == 0 ? 1u : shotsPerBarrel;
    if (currentBarrel >= normalizedCount) {
        currentBarrel = 0;
        shotsRemainingForCurrentBarrel = normalizedShots;
    }
    if (shotsRemainingForCurrentBarrel == 0) {
        shotsRemainingForCurrentBarrel = normalizedShots;
    }
    const uint32_t selected = currentBarrel + 1u;
    if (--shotsRemainingForCurrentBarrel == 0) {
        ++currentBarrel;
        shotsRemainingForCurrentBarrel = normalizedShots;
    }
    return selected;
}

class WeaponStore {
public:
    static WeaponStore& instance();

    void clear();
    bool loadFromIni(
        const container::String& filePath,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    const WeaponAuthoringTemplate* find(const container::String& name) const;
    [[nodiscard]] const container::HashMap<container::String, WeaponAuthoringTemplate>&
    all() const noexcept { return m_weapons; }

private:
    container::HashMap<container::String, WeaponAuthoringTemplate> m_weapons;
};

} // namespace game
