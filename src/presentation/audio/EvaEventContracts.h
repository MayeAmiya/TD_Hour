#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::audio {

// Stable Zero Hour EVA vocabulary shared by content compilation, game-side
// typed events and the detached audio scheduler. Authored strings never cross
// this boundary.
enum class EvaEventType : uint8_t {
    LowPower,
    InsufficientFunds,
    SuperweaponDetectedOwnParticleCannon,
    SuperweaponDetectedOwnNuke,
    SuperweaponDetectedOwnScudStorm,
    SuperweaponDetectedAllyParticleCannon,
    SuperweaponDetectedAllyNuke,
    SuperweaponDetectedAllyScudStorm,
    SuperweaponDetectedEnemyParticleCannon,
    SuperweaponDetectedEnemyNuke,
    SuperweaponDetectedEnemyScudStorm,
    SuperweaponLaunchedOwnParticleCannon,
    SuperweaponLaunchedOwnNuke,
    SuperweaponLaunchedOwnScudStorm,
    SuperweaponLaunchedAllyParticleCannon,
    SuperweaponLaunchedAllyNuke,
    SuperweaponLaunchedAllyScudStorm,
    SuperweaponLaunchedEnemyParticleCannon,
    SuperweaponLaunchedEnemyNuke,
    SuperweaponLaunchedEnemyScudStorm,
    SuperweaponReadyOwnParticleCannon,
    SuperweaponReadyOwnNuke,
    SuperweaponReadyOwnScudStorm,
    SuperweaponReadyAllyParticleCannon,
    SuperweaponReadyAllyNuke,
    SuperweaponReadyAllyScudStorm,
    SuperweaponReadyEnemyParticleCannon,
    SuperweaponReadyEnemyNuke,
    SuperweaponReadyEnemyScudStorm,
    BuildingLost,
    BaseUnderAttack,
    AllyUnderAttack,
    BeaconDetected,
    EnemyBlackLotusDetected,
    EnemyJarmenKellDetected,
    EnemyColonelBurtonDetected,
    OwnBlackLotusDetected,
    OwnJarmenKellDetected,
    OwnColonelBurtonDetected,
    UnitLost,
    GeneralLevelUp,
    VehicleStolen,
    BuildingStolen,
    CashStolen,
    UpgradeComplete,
    BuildingBeingStolen,
    BuildingSabotaged,
    SuperweaponLaunchedOwnGpsScrambler,
    SuperweaponLaunchedAllyGpsScrambler,
    SuperweaponLaunchedEnemyGpsScrambler,
    SuperweaponLaunchedOwnSneakAttack,
    SuperweaponLaunchedAllySneakAttack,
    SuperweaponLaunchedEnemySneakAttack,
    Count,
};

inline constexpr size_t kEvaEventTypeCount =
    static_cast<size_t>(EvaEventType::Count);

struct EvaPresentationPolicy final {
    EvaEventType type = EvaEventType::Count;
    uint32_t priority = 1;
    uint32_t cooldownFrames = 0;
    uint32_t expirationFrames = 0;
};

} // namespace engine::audio
