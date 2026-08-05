#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include "core/data/ini/GeneralsIniParser.h"
#include "core/math/fixed/q32_32.h"

namespace game
{

// These flags intentionally retain the authored RefCode vocabulary, but they
// are plain fixed-width masks instead of BitFlags/global state.  A live ECS
// object owns only its active mask; the profile below is immutable content.
enum class WeaponSetCondition : uint8_t
{
    Veteran = 0,
    Elite,
    Hero,
    PlayerUpgrade,
    CrateUpgradeOne,
    CrateUpgradeTwo,
    VehicleHijack,
    CarBomb,
    MineClearingDetail,
    Rider1,
    Rider2,
    Rider3,
    Rider4,
    Rider5,
    Rider6,
    Rider7,
    Rider8,
    Count,
};

using WeaponSetConditionMask = uint32_t;

[[nodiscard]] constexpr WeaponSetConditionMask weaponSetConditionBit(WeaponSetCondition condition) noexcept
{
    return WeaponSetConditionMask{1} << static_cast<uint8_t>(condition);
}

inline constexpr WeaponSetConditionMask kAllWeaponSetConditions =
    (WeaponSetConditionMask{1} << static_cast<uint8_t>(WeaponSetCondition::Count)) - 1U;

enum class ArmorSetCondition : uint8_t
{
    Veteran = 0,
    Elite,
    Hero,
    PlayerUpgrade,
    WeakVersusBaseDefenses,
    SecondLife,
    CrateUpgradeOne,
    CrateUpgradeTwo,
    Count,
};

using ArmorSetConditionMask = uint32_t;

[[nodiscard]] constexpr ArmorSetConditionMask armorSetConditionBit(ArmorSetCondition condition) noexcept
{
    return ArmorSetConditionMask{1} << static_cast<uint8_t>(condition);
}

inline constexpr ArmorSetConditionMask kAllArmorSetConditions =
    (ArmorSetConditionMask{1} << static_cast<uint8_t>(ArmorSetCondition::Count)) - 1U;

// RefCode's WeaponSlotType order is significant: most target selection walks
// TERTIARY -> PRIMARY so the authored primary slot wins damage-score ties.
enum class WeaponSlot : uint8_t
{
    Primary = 0,
    Secondary,
    Tertiary,
    Count,
};

inline constexpr std::size_t kWeaponSlotCount = static_cast<std::size_t>(WeaponSlot::Count);

// These mirror CommandSourceType only for weapon auto-selection.  Commands
// themselves stay in the command subsystem; combat has no dependency on it.
enum class WeaponCommandSource : uint8_t
{
    Player = 0,
    Script,
    AI,
    Dozer,
    DefaultSwitchWeapon,
    Count,
};

using WeaponAutoChooseSourceMask = uint32_t;

[[nodiscard]] constexpr WeaponAutoChooseSourceMask weaponAutoChooseSourceBit(WeaponCommandSource source) noexcept
{
    return WeaponAutoChooseSourceMask{1} << static_cast<uint8_t>(source);
}

// The original WeaponTemplateSet defaults every slot to 0xffffffff, not only
// the currently known command bits. Preserve that content behavior here.
inline constexpr WeaponAutoChooseSourceMask kAllWeaponAutoChooseSources = 0xffffffffU;

struct CombatProfileParseStatus final
{
    bool success = true;
    container::String message;
};

[[nodiscard]] std::optional<WeaponSetCondition> tryParseWeaponSetCondition(container::StringView token) noexcept;
[[nodiscard]] std::optional<ArmorSetCondition> tryParseArmorSetCondition(container::StringView token) noexcept;
[[nodiscard]] std::optional<WeaponSlot> tryParseWeaponSlot(container::StringView token) noexcept;
[[nodiscard]] std::optional<WeaponCommandSource> tryParseWeaponCommandSource(container::StringView token) noexcept;

// Applies one legacy bit-string value to the supplied mask.  `NONE`, normal
// tokens, and +/- edits follow BitFlags::parse and INI::parseBitString32:
// normal tokens replace the old mask; +/- edits modify it in place.
[[nodiscard]] CombatProfileParseStatus applyWeaponSetConditions(container::StringView text,
                                                                WeaponSetConditionMask& inOutMask);
[[nodiscard]] CombatProfileParseStatus applyArmorSetConditions(container::StringView text, ArmorSetConditionMask& inOutMask);
[[nodiscard]] CombatProfileParseStatus applyWeaponAutoChooseSources(container::StringView text,
                                                                    WeaponAutoChooseSourceMask& inOutMask);

struct WeaponSlotProfile final
{
    // Empty means the legacy authored value was `None` (or the slot was not
    // declared). Names are content identities, never pointers into a mutable
    // WeaponStore; a later frozen content registry resolves them once.
    container::String weaponTemplateName;
    WeaponAutoChooseSourceMask autoChooseSources = kAllWeaponAutoChooseSources;
    // PreferredAgainst requires every authored bit, matching WeaponSetData.
    ObjectKindOfMask preferredAgainstKinds{};

    [[nodiscard]] bool hasWeapon() const noexcept
    {
        return !weaponTemplateName.empty();
    }
    [[nodiscard]] bool allowsAutoChoose(WeaponCommandSource source) const noexcept;
};

struct WeaponSetProfile final
{
    WeaponSetConditionMask conditions = 0;
    container::Array<WeaponSlotProfile, kWeaponSlotCount> slots;
    bool shareWeaponReloadTime = false;
    bool weaponLockSharedAcrossSets = false;
};

struct ArmorSetProfile final
{
    ArmorSetConditionMask conditions = 0;
    // Empty means no armor/DamageFX. They remain symbolic keys so a match is
    // safe across content reload boundaries and for immutable session data.
    container::String armorTemplateName;
    container::String damageFxName;
};

enum class WeaponChoiceCriterion : uint8_t
{
    MostDamage,
    LongestRange,
};

// Dynamic facts owned by the future Weapon/Combat components.  The immutable
// profile never stores ammo, cooldowns, target pointers, or an EnTT entity.
struct WeaponSlotSelectionCandidate final
{
    bool present = false;
    bool outOfAmmo = false;
    bool autoReloadsClip = false;
    bool canTarget = true;
    bool withinTargetPitch = true;
    bool readyToFire = false;
    bool permitsZeroDamage = false;
    bool preferredAgainstTarget = false;
    math::q32_32 estimatedDamage{};
    math::q32_32 attackRange{};
};

struct WeaponSlotSelection final
{
    WeaponSlot slot = WeaponSlot::Primary;
    bool found = false;
    bool usesReadyWeapon = false;
    bool heldByLock = false;
};

// Pure compatibility selection for a previously selected WeaponSetProfile.
// It preserves legacy readiness-before-score behavior and slot tie breaking;
// the caller supplies target legality/damage estimates from CombatSystem.
[[nodiscard]] WeaponSlotSelection chooseBestWeaponSlot(
    const WeaponSetProfile& profile,
    const container::Array<WeaponSlotSelectionCandidate, kWeaponSlotCount>& candidates,
    WeaponCommandSource commandSource,
    WeaponChoiceCriterion criterion,
    bool hasTarget,
    std::optional<WeaponSlot> lockedSlot = std::nullopt) noexcept;

class CombatProfile;

enum class CombatProfileInheritanceMode : uint8_t
{
    // Matches ThingTemplate's m_weaponsCopiedFromDefault /
    // m_armorCopiedFromDefault behavior: the first local set replaces that
    // family inherited from DefaultThingTemplate.
    ReplaceInheritedOnFirstLocalSet,
    // Explicit option for a future overlay format that deliberately appends.
    AppendToInherited,
};

struct CombatProfileCompileOptions final
{
    const CombatProfile* inherited = nullptr;
    CombatProfileInheritanceMode inheritanceMode = CombatProfileInheritanceMode::ReplaceInheritedOnFirstLocalSet;
};

enum class CombatProfileDiagnosticSeverity : uint8_t
{
    Warning,
    Error,
};

struct CombatProfileDiagnostic final
{
    CombatProfileDiagnosticSeverity severity = CombatProfileDiagnosticSeverity::Warning;
    container::String message;
};

struct CombatProfileCompileResult;

// Frozen, pointer-free combat content. Publish it as shared_ptr<const> from
// ObjectArchetype/content snapshots; all runtime queries are read-only and do
// not create legacy SparseMatchFinder caches on simulation threads.
class CombatProfile final
{
public:
    [[nodiscard]] container::Span<const WeaponSetProfile> weaponSets() const noexcept;
    [[nodiscard]] container::Span<const ArmorSetProfile> armorSets() const noexcept;
    [[nodiscard]] const WeaponSetProfile* findBestWeaponSet(WeaponSetConditionMask activeConditions) const noexcept;
    [[nodiscard]] const ArmorSetProfile* findBestArmorSet(ArmorSetConditionMask activeConditions) const noexcept;
    [[nodiscard]] bool hasAnyWeapons() const noexcept;

private:
    CombatProfile(container::Vector<WeaponSetProfile> weaponSets, container::Vector<ArmorSetProfile> armorSets) noexcept;

    container::Vector<WeaponSetProfile> m_weaponSets;
    container::Vector<ArmorSetProfile> m_armorSets;

    friend CombatProfileCompileResult compileCombatProfile(container::Span<const IniBlock> directObjectChildren,
                                                           CombatProfileCompileOptions options);
};

struct CombatProfileCompileResult final
{
    container::SharedPtr<const CombatProfile> profile;
    container::Vector<CombatProfileDiagnostic> diagnostics;
    bool hasErrors = false;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return profile != nullptr && !hasErrors;
    }
};

// `directObjectChildren` is normally `IniBlock::children` from one fully
// resolved Object recipe. This deliberately does not mutate ThingTemplate:
// its caller can compile then attach the shared immutable result to the
// eventual ObjectArchetype in one isolated integration change.
[[nodiscard]] CombatProfileCompileResult compileCombatProfile(container::Span<const IniBlock> directObjectChildren,
                                                              CombatProfileCompileOptions options = {});

} // namespace game
