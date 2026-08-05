#pragma once

#include "core/container/container_types.h"
#include <cstdint>
#include <optional>
namespace game {

// Kept in game-domain data because INI ModelCondition states select playback
// semantics. GameRenderExtraction maps it to the renderer's detached enum.
enum class ModelAnimationMode : uint8_t {
    Manual,
    Loop,
    Once,
    LoopPingPong,
    LoopBackwards,
    OnceBackwards,
};

// Playback-start and cross-state continuity flags are part of the compact
// model-condition contract shared by authored visual rules and live ECS
// presentation state.  Keep them beside ModelAnimationMode so low-level
// object components do not need to include the full ThingTemplate graph.
enum class ModelAnimationFlag : uint16_t {
    RandomStart = 1u << 0u,
    StartFrameFirst = 1u << 1u,
    StartFrameLast = 1u << 2u,
    AdjustHeightByConstructionPercent = 1u << 3u,
    PristineBonePositionInFinalFrame = 1u << 4u,
    MaintainFrameAcrossStates = 1u << 5u,
    RestartAnimationWhenComplete = 1u << 6u,
    MaintainFrameAcrossStates2 = 1u << 7u,
    MaintainFrameAcrossStates3 = 1u << 8u,
    MaintainFrameAcrossStates4 = 1u << 9u,
};

using ModelAnimationFlags = uint16_t;

[[nodiscard]] constexpr ModelAnimationFlags modelAnimationFlagBit(
    ModelAnimationFlag flag) noexcept {
    return static_cast<ModelAnimationFlags>(flag);
}

// RefCode ModelCondition flag order (118 named bits). Matches
// kRefCodeConditionNames. Runtime paths must use this enum (or mask bits);
// string names are load/compile boundary only.
enum class ModelConditionFlag : uint32_t {
    Toppled = 0,
    FrontCrushed,
    BackCrushed,
    Damaged,
    ReallyDamaged,
    Rubble,
    SpecialDamaged,
    Night,
    Snow,
    Parachuting,
    Garrisoned,
    EnemyNear,
    WeaponsetVeteran,
    WeaponsetElite,
    WeaponsetHero,
    WeaponsetCrateUpgradeOne,
    WeaponsetCrateUpgradeTwo,
    WeaponsetPlayerUpgrade,
    Door1Opening,
    Door1Closing,
    Door1WaitingOpen,
    Door1WaitingToClose,
    Door2Opening,
    Door2Closing,
    Door2WaitingOpen,
    Door2WaitingToClose,
    Door3Opening,
    Door3Closing,
    Door3WaitingOpen,
    Door3WaitingToClose,
    Door4Opening,
    Door4Closing,
    Door4WaitingOpen,
    Door4WaitingToClose,
    Attacking,
    PreattackA,
    FiringA,
    BetweenFiringShotsA,
    ReloadingA,
    PreattackB,
    FiringB,
    BetweenFiringShotsB,
    ReloadingB,
    PreattackC,
    FiringC,
    BetweenFiringShotsC,
    ReloadingC,
    TurretRotate,
    PostCollapse,
    Moving,
    Dying,
    AwaitingConstruction,
    PartiallyConstructed,
    ActivelyBeingConstructed,
    Prone,
    FreeFall,
    ActivelyConstructing,
    ConstructionComplete,
    RadarExtending,
    RadarUpgraded,
    Panicking,
    Aflame,
    Smoldering,
    Burned,
    Docking,
    DockingBeginning,
    DockingActive,
    DockingEnding,
    Carrying,
    Flooded,
    Loaded,
    JetAfterburner,
    JetExhaust,
    Packing,
    Unpacking,
    Deployed,
    OverWater,
    PowerPlantUpgraded,
    Climbing,
    Sold,
    Surrender,
    Rappelling,
    Armed,
    PowerPlantUpgrading,
    SpecialCheering,
    ContinuousFireSlow,
    ContinuousFireMean,
    ContinuousFireFast,
    RaisingFlag,
    Captured,
    ExplodedFlailing,
    ExplodedBouncing,
    Splatted,
    UsingWeaponA,
    UsingWeaponB,
    UsingWeaponC,
    Preorder,
    CenterToLeft,
    LeftToCenter,
    CenterToRight,
    RightToCenter,
    Rider1,
    Rider2,
    Rider3,
    Rider4,
    Rider5,
    Rider6,
    Rider7,
    Rider8,
    StunnedFlailing,
    Stunned,
    SecondLife,
    Jammed,
    ArmorsetCrateUpgradeOne,
    ArmorsetCrateUpgradeTwo,
    User1,
    User2,
    Disguised,
    Count
};

constexpr uint32_t kModelConditionFlagCount =
    static_cast<uint32_t>(ModelConditionFlag::Count);

[[nodiscard]] constexpr uint32_t modelConditionFlagIndex(
    ModelConditionFlag flag) noexcept {
    return static_cast<uint32_t>(flag);
}

// Keep two explicit 64-bit words rather than narrowing the visual contract
// to a legacy uint64 (covers the full 118-flag table).
struct ModelConditionMask {
    container::Array<uint64_t, 2> words{};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] uint32_t intersectionCount(const ModelConditionMask& other) const noexcept;
    [[nodiscard]] uint32_t extraneousCountAgainst(const ModelConditionMask& active) const noexcept;
    void clear(const ModelConditionMask& mask) noexcept;
    void set(uint32_t index, bool value = true) noexcept;

    void set(ModelConditionFlag flag, bool value = true) noexcept {
        set(modelConditionFlagIndex(flag), value);
    }
};

[[nodiscard]] constexpr ModelConditionMask modelConditionMaskOf(
    ModelConditionFlag flag) noexcept {
    ModelConditionMask mask{};
    const uint32_t index = modelConditionFlagIndex(flag);
    if (index < kModelConditionFlagCount) {
        mask.words[index / 64u] |= (uint64_t{1} << (index % 64u));
    }
    return mask;
}

[[nodiscard]] constexpr ModelConditionMask modelConditionMaskUnion(
    ModelConditionMask left, ModelConditionMask right) noexcept {
    left.words[0] |= right.words[0];
    left.words[1] |= right.words[1];
    return left;
}

template <typename... Flags>
[[nodiscard]] constexpr ModelConditionMask modelConditionMaskOf(
    ModelConditionFlag first, ModelConditionFlag second, Flags... rest) noexcept {
    return modelConditionMaskUnion(
        modelConditionMaskOf(first),
        modelConditionMaskOf(second, rest...));
}

// Builds the exact transient weapon-condition replacement performed by
// RefCode Object::setFiringConditionForCurrentWeapon().  Keep this beside the
// condition contract so combat launch-pose resolution, FireFX binding and the
// later model-condition authority cannot drift into three slot mappings.
[[nodiscard]] inline ModelConditionMask weaponFiringModelConditions(
    ModelConditionMask current, uint32_t slot) noexcept {
    ModelConditionMask owned;
    ModelConditionFlag firing = ModelConditionFlag::FiringA;
    ModelConditionFlag usingWeapon = ModelConditionFlag::UsingWeaponA;
    switch (slot) {
    case 0:
        owned = modelConditionMaskOf(
            ModelConditionFlag::PreattackA, ModelConditionFlag::FiringA,
            ModelConditionFlag::BetweenFiringShotsA,
            ModelConditionFlag::ReloadingA,
            ModelConditionFlag::UsingWeaponA);
        break;
    case 1:
        owned = modelConditionMaskOf(
            ModelConditionFlag::PreattackB, ModelConditionFlag::FiringB,
            ModelConditionFlag::BetweenFiringShotsB,
            ModelConditionFlag::ReloadingB,
            ModelConditionFlag::UsingWeaponB);
        firing = ModelConditionFlag::FiringB;
        usingWeapon = ModelConditionFlag::UsingWeaponB;
        break;
    case 2:
        owned = modelConditionMaskOf(
            ModelConditionFlag::PreattackC, ModelConditionFlag::FiringC,
            ModelConditionFlag::BetweenFiringShotsC,
            ModelConditionFlag::ReloadingC,
            ModelConditionFlag::UsingWeaponC);
        firing = ModelConditionFlag::FiringC;
        usingWeapon = ModelConditionFlag::UsingWeaponC;
        break;
    default:
        return current;
    }
    current.clear(owned);
    current.set(firing);
    current.set(usingWeapon);
    return current;
}

// Load/compile boundary only. Runtime must use ModelConditionFlag.
[[nodiscard]] ModelConditionMask parseModelConditionMask(container::StringView names);
[[nodiscard]] std::optional<uint32_t> tryParseModelConditionFlag(container::StringView name) noexcept;
[[nodiscard]] std::optional<ModelConditionFlag> tryParseModelConditionFlagEnum(
    container::StringView name) noexcept;
[[nodiscard]] container::StringView modelConditionFlagName(ModelConditionFlag flag) noexcept;

} // namespace game
