#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/plan/economy/ObjectUpgradePlanTypes.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/data/base/SpecialPowerType.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;
namespace terrain { struct MapVisibilitySnapshot; }

struct ObjectPropagandaTowerRule final {
    uint32_t authoredOrder = 0;
    math::q32_32 radius{int32_t{1}};
    uint32_t scanDelayMilliseconds = 100;
    math::q32_32 healPercentPerSecond =
        math::q32_32::from_fraction(1, 100);
    math::q32_32 upgradedHealPercentPerSecond =
        math::q32_32::from_fraction(2, 100);
    container::String pulseFx;
    container::String upgradedPulseFx;
    container::String upgradeRequired;
    engine::UpgradeContentId upgradeRequiredId =
        engine::INVALID_UPGRADE_CONTENT_ID;
    bool affectsSelf = false;
};

struct ObjectAssistedTargetingRule final {
    uint32_t authoredOrder = 0;
    uint32_t assistingClipSize = 0;
    WeaponSlot assistingWeaponSlot = WeaponSlot::Primary;
    container::String laserFromAssisted;
    container::String laserToTarget;
};

struct ObjectDeployStyleRule final {
    uint32_t authoredOrder = 0;
    uint32_t packMilliseconds = 0;
    uint32_t unpackMilliseconds = 0;
    bool resetTurretBeforePacking = false;
    bool turretsFunctionOnlyWhenDeployed = false;
    bool turretsMustCenterBeforePacking = false;
    bool manualDeployAnimations = false;
};

struct ObjectToppleRule final {
    uint32_t authoredOrder = 0;
    container::String toppleFx;
    container::String bounceFx;
    container::String stumpName;
    math::q32_32 initialVelocityPercent =
        math::q32_32::from_fraction(1, 100);
    math::q32_32 initialAccelerationPercent =
        math::q32_32::from_fraction(1, 100);
    math::q32_32 bounceVelocityPercent =
        math::q32_32::from_fraction(3, 10);
    bool killWhenStartToppling = false;
    bool killWhenFinishedToppling = true;
    bool killStumpWhenToppled = false;
    bool toppleLeftOrRightOnly = false;
    bool reorientToppledRubble = false;
};

enum class ObjectBattlePlanStatus : uint8_t {
    None,
    Bombardment,
    HoldTheLine,
    SearchAndDestroy,
};

struct ObjectBattlePlanRule final {
    uint32_t authoredOrder = 0;
    container::String specialPowerTemplate;
    uint32_t bombardmentAnimationMilliseconds = 0;
    uint32_t holdTheLineAnimationMilliseconds = 0;
    uint32_t searchAndDestroyAnimationMilliseconds = 0;
    uint32_t transitionIdleMilliseconds = 0;
    uint32_t paralyzeMilliseconds = 0;
    ObjectKindOfMask validMemberKinds{};
    ObjectKindOfMask invalidMemberKinds{};
    math::q32_32 holdTheLineArmorDamageScalar{int32_t{1}};
    math::q32_32 searchAndDestroySightRangeScalar{int32_t{1}};
    math::q32_32 strategyCenterSearchSightScalar{int32_t{1}};
    math::q32_32 strategyCenterHoldHealthScalar{int32_t{1}};
    ObjectMaxHealthChangeType strategyCenterHoldHealthChangeType =
        ObjectMaxHealthChangeType::PreserveRatio;
    container::String bombardmentUnpackSound;
    container::String bombardmentPackSound;
    container::String bombardmentMessageLabel;
    container::String bombardmentAnnouncement;
    container::String holdTheLineUnpackSound;
    container::String holdTheLinePackSound;
    container::String holdTheLineMessageLabel;
    container::String holdTheLineAnnouncement;
    container::String searchAndDestroyUnpackSound;
    container::String searchAndDestroyIdleLoopSound;
    container::String searchAndDestroyPackSound;
    container::String searchAndDestroyMessageLabel;
    container::String searchAndDestroyAnnouncement;
    // Retail retains this field although ShroudRevealToAllRange supersedes
    // creation of the helper object. Keeping the resolved name makes the
    // unsupported/no-op decision explicit at the frozen content boundary.
    container::String visionObjectName;
    bool strategyCenterDetectsStealth = true;
};

struct ObjectSpecialAbilityUpdateRule final {
    uint32_t authoredOrder = 0;
    container::String specialPowerTemplate;
    math::q32_32 startAbilityRange{10'000'000};
    math::q32_32 abilityAbortRange{10'000'000};
    uint32_t preparationMilliseconds = 0;
    uint32_t persistentPrepMilliseconds = 0;
    uint32_t packMilliseconds = 0;
    uint32_t unpackMilliseconds = 0;
    uint32_t preTriggerUnstealthMilliseconds = 0;
    uint32_t effectDurationMilliseconds = 0;
    int32_t effectValue = 1;
    int32_t awardExperienceForTriggering = 0;
    int32_t skillPointsForTriggering = -1;
    container::String specialObject;
    container::String specialObjectAttachToBone;
    container::String disableFxParticleSystem;
    container::String packSound;
    container::String unpackSound;
    container::String prepSoundLoop;
    container::String triggerSound;
    uint32_t maximumSpecialObjects = 1;
    math::q32_32 fleeRangeAfterCompletion{};
    math::q32_32 packUnpackVariationFactor{};
    bool skipPackingWithNoTarget = false;
    bool specialObjectsPersistent = false;
    bool uniqueSpecialObjectTargets = false;
    bool specialObjectsPersistWhenOwnerDies = false;
    bool alwaysValidateSpecialObjects = false;
    bool flipOwnerAfterPacking = false;
    bool flipOwnerAfterUnpacking = false;
    bool doCaptureFx = false;
    bool loseStealthOnTrigger = false;
    bool approachRequiresLineOfSight = true;
    bool needToFaceTarget = true;
    bool persistenceRequiresRecharge = false;
};

struct ObjectCommandButtonHuntRule final {
    uint32_t authoredOrder = 0;
    uint32_t scanRateMilliseconds = 1000;
    math::q32_32 scanRange{9999};
};

struct ObjectTacticalPlan final {
    container::Vector<ObjectPropagandaTowerRule> propagandaTowers;
    container::Vector<ObjectAssistedTargetingRule> assistedTargeting;
    container::Vector<ObjectDeployStyleRule> deployStyles;
    container::Vector<ObjectToppleRule> topple;
    container::Vector<ObjectBattlePlanRule> battlePlans;
    container::Vector<ObjectSpecialAbilityUpdateRule> specialAbilities;
    container::Vector<ObjectCommandButtonHuntRule> commandButtonHunts;
    container::Vector<uint32_t> wanderAuthoredOrders;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectTacticalPlan>
compileObjectTacticalPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game
