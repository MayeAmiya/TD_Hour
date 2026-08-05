#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/simulation/economy/ObjectUpgradeDetail.h"

namespace engine
{
namespace object_upgrade_detail
{
using HealthScalar = ObjectHealthComponent::Scalar;

const HealthScalar kHealthZero{};


using container::asciiEqualIgnoreCase;

[[nodiscard]] bool completedUpgrade(
    UpgradeContentId upgrade,
    const UpgradeMask& playerCompletedUpgrades,
    const UpgradeMask& objectCompletedUpgrades) noexcept
{
    return upgradeMaskTest(playerCompletedUpgrades, upgrade) ||
           upgradeMaskTest(objectCompletedUpgrades, upgrade);
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds, uint32_t framesPerSecond) noexcept
{
    if (milliseconds == 0)
        return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

[[nodiscard]] game::ModelConditionMask conditionMask(container::StringView names)
{
    return game::parseModelConditionMask(names);
}

void publishUpgradeConditions(ecs::registry& registry,
                              ecs::entity entity,
                              const game::ModelConditionMask& clearMask,
                              const game::ModelConditionMask& setMask,
                              uint64_t confirmedTick) noexcept
{
    // ModelConditionUpgrade is a Drawable-side no-op in RefCode. Preserve
    // that boundary even though retaining a sparse contribution for a
    // bodyless object would otherwise be harmless.
    if (!ecs::try_get<RenderModelComponent>(registry, entity))
        return;
    publishObjectModelConditionContribution(
        registry, entity, ObjectModelConditionContributionSource::Upgrade,
        clearMask, setMask, confirmedTick);
}

void applyMaxHealthUpgrade(ecs::registry& registry, ecs::entity entity,
                           ObjectHealthComponent& health,
                           const game::ObjectUpgradeRule& rule,
                           const ObjectSimulationRules& rules,
                           RenderModelComponent* visual) noexcept
{
    const HealthScalar previousMaximum = health.maximumFixed;
    const HealthScalar nextMaximum = previousMaximum + rule.addMaxHealth;
    // Source data normally uses a positive bonus. Keep malformed content from
    // creating a non-positive Body that would bypass the normal death
    // transaction; the module remains consumed just as the legacy mux does.
    if (nextMaximum <= kHealthZero)
        return;

    const HealthScalar previousCurrent = health.currentFixed;
    HealthScalar nextCurrent = previousCurrent;
    switch (rule.maxHealthChangeType)
    {
    case game::ObjectMaxHealthChangeType::PreserveRatio:
        // Match ActiveBody::setMaxHealth exactly: divide first, then apply the
        // preserved ratio.  Fixed-point multiplication-before-division has a
        // different rounding boundary and a needlessly larger intermediate.
        nextCurrent = previousMaximum > kHealthZero
            ? nextMaximum * (previousCurrent / previousMaximum)
            : nextMaximum;
        break;
    case game::ObjectMaxHealthChangeType::AddCurrentHealthToo:
        nextCurrent = previousCurrent + (nextMaximum - previousMaximum);
        break;
    case game::ObjectMaxHealthChangeType::FullyHeal:
        nextCurrent = nextMaximum;
        break;
    case game::ObjectMaxHealthChangeType::SameCurrentHealth:
        break;
    }
    nextCurrent = HealthScalar::min(nextMaximum, HealthScalar::max(kHealthZero, nextCurrent));
    health.previousFixed = previousCurrent;
    health.maximumFixed = nextMaximum;
    health.initialFixed = nextMaximum;
    health.currentFixed = nextCurrent;
    health.damageState = objectBodyDamageStateFor(nextCurrent, nextMaximum,
                                                  rules);
    if (visual)
        projectObjectBodyDamageVisual(
            registry, entity, health.damageState, *visual);
}

void applyLocomotorSetUpgrade(ecs::registry& registry, ecs::entity entity)
{
    ObjectLocomotorSetUpgradeComponent* upgrade =
        ecs::try_get<ObjectLocomotorSetUpgradeComponent>(registry, entity);
    ObjectLocomotionComponent* locomotion =
        ecs::try_get<ObjectLocomotionComponent>(registry, entity);
    if (!upgrade || !locomotion || upgrade->active)
        return;

    if (upgrade->upgraded.empty())
        return;
    locomotion->profiles = upgrade->upgraded;
    const game::FrozenLocomotorTemplate& source = locomotion->profiles.front();
    locomotion->templateName = source.name;
    locomotion->surfaces = source.surfaces;
    locomotion->appearance = source.appearance;
    locomotion->zAxisBehavior = source.zAxisBehavior;
    locomotion->groupPriority = source.groupPriority;
    locomotion->circlingRadius = source.fixed.circlingRadius;
    locomotion->extra2DFrictionPerSecond =
        source.fixed.extra2DFrictionPerSecond;
    locomotion->maximumThrustAngleRadians =
        source.fixed.maximumThrustAngleRadians;
    locomotion->accelerationPitchLimitRadians =
        source.fixed.accelerationPitchLimitRadians;
    locomotion->decelerationPitchLimitRadians =
        source.fixed.decelerationPitchLimitRadians;
    locomotion->bounceAngularVelocityRadiansPerSecond =
        source.fixed.bounceAngularVelocityRadiansPerSecond;
    locomotion->pitchStiffness = source.fixed.pitchStiffness;
    locomotion->rollStiffness = source.fixed.rollStiffness;
    locomotion->pitchDamping = source.fixed.pitchDamping;
    locomotion->rollDamping = source.fixed.rollDamping;
    locomotion->thrustRoll = source.fixed.thrustRoll;
    locomotion->thrustWobbleRate = source.fixed.thrustWobbleRate;
    locomotion->thrustMinimumWobble = source.fixed.thrustMinimumWobble;
    locomotion->thrustMaximumWobble = source.fixed.thrustMaximumWobble;
    locomotion->pitchByZVelocityFactor =
        source.fixed.pitchByZVelocityFactor;
    locomotion->forwardVelocityPitchFactor =
        source.fixed.forwardVelocityPitchFactor;
    locomotion->lateralVelocityRollFactor =
        source.fixed.lateralVelocityRollFactor;
    locomotion->forwardAccelerationPitchFactor =
        source.fixed.forwardAccelerationPitchFactor;
    locomotion->lateralAccelerationRollFactor =
        source.fixed.lateralAccelerationRollFactor;
    locomotion->uniformAxialDamping = source.fixed.uniformAxialDamping;
    locomotion->turnPivotOffset = source.fixed.turnPivotOffset;
    locomotion->maximumWheelExtension = source.fixed.maximumWheelExtension;
    locomotion->maximumWheelCompression =
        source.fixed.maximumWheelCompression;
    locomotion->frontWheelTurnAngleRadians =
        source.fixed.frontWheelTurnAngleRadians;
    locomotion->wanderWidthFactor = source.fixed.wanderWidthFactor;
    locomotion->wanderLengthFactor = source.fixed.wanderLengthFactor;
    locomotion->wanderAboutPointRadius =
        source.fixed.wanderAboutPointRadius;
    locomotion->rudderCorrectionDegree =
        source.fixed.rudderCorrectionDegree;
    locomotion->rudderCorrectionRate = source.fixed.rudderCorrectionRate;
    locomotion->elevatorCorrectionDegree =
        source.fixed.elevatorCorrectionDegree;
    locomotion->elevatorCorrectionRate =
        source.fixed.elevatorCorrectionRate;
    locomotion->airborneTargetingHeight =
        source.fixed.airborneTargetingHeight;
    locomotion->hasFiniteAirborneTargetingHeight =
        source.fixed.hasFiniteAirborneTargetingHeight;
    locomotion->closeEnoughDistance3D = source.closeEnoughDistance3D;
    locomotion->stickToGround = source.stickToGround;
    locomotion->canMoveBackwards = source.canMoveBackwards;
    locomotion->locomotorWorksWhenDead = source.locomotorWorksWhenDead;
    locomotion->allowMotiveForceWhileAirborne = source.allowMotiveForceWhileAirborne;
    locomotion->apply2DFrictionWhenAirborne = source.apply2DFrictionWhenAirborne;
    locomotion->downhillOnly = source.downhillOnly;
    locomotion->hasSuspension = source.hasSuspension;

    // A set upgrade is another locomotor-template ingress. Refresh the fixed
    // configuration in the same transaction; the retained runtime speed and
    // goal remain fixed and are never reconstructed from their float mirrors.
    locomotion->maximumSpeed = source.fixed.maximumSpeed;
    locomotion->damagedMaximumSpeed = source.fixed.damagedMaximumSpeed;
    locomotion->maximumTurnRate = source.fixed.maximumTurnRate;
    locomotion->damagedMaximumTurnRate =
        source.fixed.damagedMaximumTurnRate;
    locomotion->acceleration = source.fixed.acceleration;
    locomotion->damagedAcceleration = source.fixed.damagedAcceleration;
    locomotion->lift = source.fixed.lift;
    locomotion->damagedLift = source.fixed.damagedLift;
    locomotion->braking = source.fixed.braking;
    locomotion->minimumSpeed = source.fixed.minimumSpeed;
    locomotion->minimumTurnSpeed = source.fixed.minimumTurnSpeed;
    locomotion->preferredHeightFixed = source.fixed.preferredHeight;
    locomotion->preferredHeightDampingFixed =
        source.fixed.preferredHeightDamping;
    locomotion->speedLimitZ = source.fixed.speedLimitZ;
    locomotion->closeEnough = source.fixed.closeEnough;
    locomotion->slideIntoPlace =
        source.fixed.slideIntoPlaceMilliseconds;
    locomotion->accelerationIsInfinite =
        source.fixed.accelerationIsInfinite;
    locomotion->damagedAccelerationIsInfinite =
        source.fixed.damagedAccelerationIsInfinite;
    locomotion->brakingIsInfinite = source.fixed.brakingIsInfinite;
    locomotion->hasFiniteBraking = source.fixed.hasFiniteBraking;
    locomotion->hasFiniteSpeedLimitZ =
        source.fixed.hasFiniteSpeedLimitZ;
    locomotion->preferredHeightIsLowest =
        source.fixed.preferredHeightIsLowest;

    // AIUpdate::chooseLocomotorSetExplicit destroys the old Locomotor
    // instances, but PhysicsBehavior owns and retains the object's velocity.
    // Preserve that physical speed and the AI/order intent. MOVING_BACKWARDS
    // is locomotor-instance state, so the fresh upgraded instance starts with
    // that flag clear.
    locomotion->movingBackward = false;
    upgrade->active = true;
    if (upgrade->revision != std::numeric_limits<uint64_t>::max())
        ++upgrade->revision;
}

void beginPowerPlantExtension(ecs::registry& registry,
                              ecs::entity entity,
                              ObjectPowerPlantComponent& powerPlant,
                              const ObjectSimulationRules& rules,
                              uint64_t confirmedTick)
{
    if (powerPlant.state == ObjectPowerPlantRodState::Extending ||
        powerPlant.state == ObjectPowerPlantRodState::Extended)
    {
        return;
    }
    static const game::ModelConditionMask upgrading = conditionMask("POWER_PLANT_UPGRADING");
    static const game::ModelConditionMask upgraded = conditionMask("POWER_PLANT_UPGRADED");
    publishUpgradeConditions(registry, entity, upgraded, upgrading,
                             confirmedTick);
    const uint64_t duration = millisecondsToTicks(powerPlant.rodsExtendMilliseconds, rules.logicFramesPerSecond);
    // UPDATE_SLEEP(0) still wakes through the next update dispatch in the
    // source engine. Keep the upgrading condition for one confirmed boundary
    // instead of changing to Extended in the same upgrade transaction.
    powerPlant.state = ObjectPowerPlantRodState::Extending;
    powerPlant.extensionCompleteTick = saturatingAdd(confirmedTick, std::max<uint64_t>(1, duration));
}

void completePowerPlantExtension(ecs::registry& registry, ecs::entity entity,
                                 ObjectPowerPlantComponent& powerPlant,
                                 uint64_t confirmedTick)
{
    static const game::ModelConditionMask upgrading = conditionMask("POWER_PLANT_UPGRADING");
    static const game::ModelConditionMask upgraded = conditionMask("POWER_PLANT_UPGRADED");
    publishUpgradeConditions(registry, entity, upgrading, upgraded,
                             confirmedTick);
    powerPlant.state = ObjectPowerPlantRodState::Extended;
}

void retractPowerPlant(ecs::registry& registry, ecs::entity entity,
                       ObjectPowerPlantComponent& powerPlant,
                       uint64_t confirmedTick)
{
    static const game::ModelConditionMask upgrading = conditionMask("POWER_PLANT_UPGRADING");
    static const game::ModelConditionMask upgraded = conditionMask("POWER_PLANT_UPGRADED");
    game::ModelConditionMask both = upgrading;
    for (size_t index = 0; index < both.words.size(); ++index)
        both.words[index] |= upgraded.words[index];
    publishUpgradeConditions(registry, entity, both, {}, confirmedTick);
    powerPlant.state = ObjectPowerPlantRodState::Retracted;
    powerPlant.extensionCompleteTick = 0;
}

void beginRadarExtension(ecs::registry& registry, ecs::entity entity,
                         ObjectRadarUpdateComponent& radar,
                         const ObjectSimulationRules& rules,
                         uint64_t confirmedTick) {
    static const game::ModelConditionMask extending =
        conditionMask("RADAR_EXTENDING");
    static const game::ModelConditionMask upgraded =
        conditionMask("RADAR_UPGRADED");
    publishUpgradeConditions(registry, entity, upgraded, extending,
                             confirmedTick);
    const uint32_t milliseconds = radar.plan
        ? radar.plan->extendMilliseconds : 0;
    const uint64_t duration = millisecondsToTicks(
        milliseconds, rules.logicFramesPerSecond);
    radar.active = true;
    radar.extensionComplete = false;
    radar.extensionCompleteTick = saturatingAdd(
        confirmedTick, duration);
}

void completeRadarExtension(ecs::registry& registry, ecs::entity entity,
                            ObjectRadarUpdateComponent& radar,
                            uint64_t confirmedTick) {
    static const game::ModelConditionMask extending =
        conditionMask("RADAR_EXTENDING");
    static const game::ModelConditionMask upgraded =
        conditionMask("RADAR_UPGRADED");
    publishUpgradeConditions(registry, entity, extending, upgraded,
                             confirmedTick);
    radar.extensionComplete = true;
    radar.extensionCompleteTick = 0;
}

void applyRule(ecs::registry& registry,
               ecs::entity entity,
               const game::ObjectUpgradeRule& rule,
               const UpgradeMask& ownerCompletedUpgrades,
               const UpgradeMask& objectCompletedUpgrades,
               const ObjectSimulationRules& rules,
               uint64_t confirmedTick,
               ObjectUpgradeExecutionContext context)
{
    switch (rule.operation)
    {
    case game::ObjectUpgradeOperation::MaxHealth:
    {
        ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (!health)
            return;
        applyMaxHealthUpgrade(
            registry, entity, *health, rule, rules,
            ecs::try_get<RenderModelComponent>(registry, entity));
        markObjectDirty(
            registry, entity,
            objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        return;
    }
    case game::ObjectUpgradeOperation::ArmorSetPlayerUpgrade:
    {
        if (rule.appliesChemicalSuitsDecal) {
            setObjectTerrainDecalKind(
                registry, entity, ObjectTerrainDecalKind::ChemSuit,
                confirmedTick, false);
        }
        ObjectCombatProfileComponent* combat = ecs::try_get<ObjectCombatProfileComponent>(registry, entity);
        if (!combat)
            return;
        combat->armorConditions |= game::armorSetConditionBit(game::ArmorSetCondition::PlayerUpgrade);
        if (ObjectArmorComponent* armor = ecs::try_get<ObjectArmorComponent>(registry, entity))
        {
            refreshResolvedObjectArmor(*combat, *armor);
        }
        return;
    }
    case game::ObjectUpgradeOperation::WeaponSetPlayerUpgrade:
    {
        if (ObjectCombatProfileComponent* combat = ecs::try_get<ObjectCombatProfileComponent>(registry, entity))
        {
            combat->weaponConditions |= game::weaponSetConditionBit(game::WeaponSetCondition::PlayerUpgrade);
        }
        if (context.content)
            static_cast<void>(refreshObjectWeaponSet(
                registry, entity, *context.content,
                rules.logicFramesPerSecond, confirmedTick));
        static const game::ModelConditionMask weaponUpgrade = conditionMask("WEAPONSET_PLAYER_UPGRADE");
        publishUpgradeConditions(registry, entity, {}, weaponUpgrade,
                                 confirmedTick);
        return;
    }
    case game::ObjectUpgradeOperation::WeaponBonusPlayerUpgrade:
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity, game::WeaponBonusCondition::PlayerUpgrade, true,
            context.content, context.random, rules.logicFramesPerSecond,
            confirmedTick));
        return;
    case game::ObjectUpgradeOperation::PowerPlant:
    {
        if (ObjectEnergyComponent* energy = ecs::try_get<ObjectEnergyComponent>(registry, entity))
        {
            const ObjectEnergyBonusSourceMask source =
                objectEnergyBonusSourceBit(ObjectEnergyBonusSource::PowerPlantUpgrade);
            if (source != 0 && energy->bonusProduction != 0)
            {
                energy->bonusProductionSources =
                    static_cast<ObjectEnergyBonusSourceMask>(energy->bonusProductionSources | source);
            }
        }
        if (ObjectPowerPlantComponent* powerPlant = ecs::try_get<ObjectPowerPlantComponent>(registry, entity))
        {
            powerPlant->extensionSources = static_cast<ObjectPowerPlantExtensionSourceMask>(
                powerPlant->extensionSources |
                objectPowerPlantExtensionSourceBit(ObjectPowerPlantExtensionSource::PowerPlantUpgrade));
            beginPowerPlantExtension(registry, entity, *powerPlant, rules, confirmedTick);
        }
        return;
    }
    case game::ObjectUpgradeOperation::StatusBits:
        static_cast<void>(ObjectStatusSystem::apply(
            registry,
            entity,
            {.setMask = rule.statusToSet, .clearMask = rule.statusToClear, .confirmedTick = confirmedTick}));
        return;
    case game::ObjectUpgradeOperation::ModelCondition:
        publishUpgradeConditions(registry, entity, {}, rule.modelCondition,
                                 confirmedTick);
        return;
    case game::ObjectUpgradeOperation::LocomotorSet:
        applyLocomotorSetUpgrade(registry, entity);
        return;
    case game::ObjectUpgradeOperation::GrantScience:
    {
        if (!context.players || !context.scienceCatalog || rule.grantScience.empty())
            return;
        const ScienceDefinition* science = context.scienceCatalog->find(rule.grantScience);
        if (!science || !science->grantable)
            return;
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
        if (!owner || !owner->player)
            return;
        // Player::grantScience bypasses price and prerequisites but still
        // rejects non-grantable sciences. The catalog check above preserves
        // that distinction; PlayerRegistry owns the idempotent insertion,
        // acquisition notification and technology revision transaction.
        static_cast<void>(context.players->grantScience(owner->player, science->name));
        return;
    }
    case game::ObjectUpgradeOperation::CommandSet:
    {
        const bool useAlternative = completedUpgrade(
            rule.triggerAltId, ownerCompletedUpgrades,
            objectCompletedUpgrades);
        const container::String& selected = useAlternative ? rule.commandSetAlt : rule.commandSet;
        ObjectCommandSetOverrideComponent* overrideState =
            ecs::try_get<ObjectCommandSetOverrideComponent>(registry, entity);
        if (!overrideState)
        {
            overrideState = &ecs::emplace<ObjectCommandSetOverrideComponent>(registry, entity);
        }
        overrideState->name = selected;
        if (overrideState->revision != std::numeric_limits<uint64_t>::max())
            ++overrideState->revision;
        overrideState->lastAppliedTick = confirmedTick;
        return;
    }
    case game::ObjectUpgradeOperation::SubObjects:
    {
        ObjectSubObjectVisibilityOverrideComponent* overrides =
            ecs::try_get<ObjectSubObjectVisibilityOverrideComponent>(registry, entity);
        if (!overrides)
            return;
        const auto setVisibility = [overrides](container::StringView name, bool visible) {
            const auto found = std::find_if(
                overrides->entries.begin(), overrides->entries.end(),
                [name](const ObjectSubObjectVisibilityOverride& entry) {
                    return asciiEqualIgnoreCase(entry.name, name);
                });
            if (found == overrides->entries.end())
                return;
            found->visible = visible;
            found->active = true;
        };
        for (const container::String& name : rule.showSubObjects)
            setVisibility(name, true);
        // Source implementation applies Show first and Hide second, so a
        // duplicate authored in both lists is hidden.
        for (const container::String& name : rule.hideSubObjects)
            setVisibility(name, false);
        if ((!rule.showSubObjects.empty() || !rule.hideSubObjects.empty()) &&
            overrides->revision != std::numeric_limits<uint64_t>::max())
        {
            ++overrides->revision;
        }
        if (!rule.showSubObjects.empty() || !rule.hideSubObjects.empty()) {
            markObjectDirty(
                registry, entity,
                ObjectDirtyDomain::RenderExtraction);
        }
        return;
    }
    case game::ObjectUpgradeOperation::ExperienceScalar:
        static_cast<void>(ObjectExperienceSystem{}.addScalar(
            registry, entity, rule.addExperienceScalar, confirmedTick));
        return;
    case game::ObjectUpgradeOperation::CostModifier:
        // Activation itself is the authoritative fact. Production resolves
        // the sparse live set at admission, so capture/deletion needs no
        // mirrored Player-side reference count or rollback callback.
        return;
    case game::ObjectUpgradeOperation::Stealth:
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.setMask = game::objectStatusBit(
                 game::ObjectStatusFlag::CanStealth),
             .confirmedTick = confirmedTick}));
        return;
    case game::ObjectUpgradeOperation::ObjectCreation:
    {
        // INI::parseObjectCreationList accepts a missing/None value as a
        // legitimate null pointer. It is a sticky no-op UpgradeMux, not a
        // malformed reference and not a reason to consume an effect sink.
        if (rule.objectCreationList.empty()) {
            return;
        }
        if (!context.content || !context.effects) {
            return;
        }
        const game::ObjectCreationListContentId content =
            context.content->findObjectCreationListId(
                rule.objectCreationList);
        if (!content ||
            !context.content->findObjectCreationList(content)) {
            return;
        }
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(registry, entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity);
        if (!identity || !identity->id || !owner || !team || !transform) {
            return;
        }
        LogicFixedVec3 velocity;
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        if (physics) {
            velocity = physics->velocityUnitsPerSecond;
        }
        const ObjectVeterancyComponent* veterancy =
            ecs::try_get<ObjectVeterancyComponent>(registry, entity);
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, entity);
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
        context.effects->queueObjectCreationListInvocation({
            .content = content,
            .source = identity->id,
            .owner = owner->player,
            .primaryTeam = team->team,
            .primaryPosition = readAuthoritativeObjectPosition(
                registry, entity, *transform),
            .sourceVelocity = velocity,
            .orientationRadians = physics && physics->ownsAttitude
                ? physics->yaw
                : readAuthoritativeObjectYaw(registry, entity, *transform),
            .pitchRadians = physics && physics->ownsAttitude
                ? physics->pitch : ObjectPhysicsComponent::Scalar{},
            .rollRadians = physics && physics->ownsAttitude
                ? physics->roll : ObjectPhysicsComponent::Scalar{},
            .veterancy = veterancy
                ? veterancy->level
                : game::ObjectVeterancyLevel::Regular,
            .authoredOrder = rule.authoredOrder,
            .emissionSequence =
                context.effects->reserveGameplaySubmissionOrdinal(),
            .confirmedTick = confirmedTick,
            .sourcePathfindLayer = terrainLayer
                ? terrainLayer->pathfindLayer
                : game::terrain::kGroundPathfindLayer,
            .sourceAirborne = airborne && airborne->isAirborne,
            .sourceOwnsFullAttitude = physics && physics->ownsAttitude,
            .resumeSourceUpgradeMux = true,
        });
        return;
    }
    case game::ObjectUpgradeOperation::ReplaceObject:
    {
        if (!context.content || !context.effects ||
            rule.replacementObject.empty() ||
            !context.content->findObjectArchetype(rule.replacementObject)) {
            return;
        }
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(registry, entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity);
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        if (!identity || !identity->id || !owner || !owner->player ||
            !team || !team->team || !transform) {
            return;
        }
        context.effects->queueObjectReplacementInvocation({
            .replacementTemplate = rule.replacementObject,
            .source = identity->id,
            .owner = owner->player,
            .primaryTeam = team->team,
            .position = readAuthoritativeObjectPosition(
                registry, entity, *transform),
            .orientationRadians = physics && physics->ownsAttitude
                ? physics->yaw
                : readAuthoritativeObjectYaw(registry, entity, *transform),
            .pitchRadians = physics && physics->ownsAttitude
                ? physics->pitch : ObjectPhysicsComponent::Scalar{},
            .rollRadians = physics && physics->ownsAttitude
                ? physics->roll : ObjectPhysicsComponent::Scalar{},
            .sourceOwnsFullAttitude = physics && physics->ownsAttitude,
            .authoredOrder = rule.authoredOrder,
            .emissionSequence =
                context.effects->reserveGameplaySubmissionOrdinal(),
            .confirmedTick = confirmedTick,
        });
        return;
    }
    case game::ObjectUpgradeOperation::ActiveShroud:
    {
        ObjectActiveShroudComponent value{
            .radius = rule.newShroudRange,
            .activatedTick = confirmedTick,
        };
        if (ObjectActiveShroudComponent* existing =
                ecs::try_get<ObjectActiveShroudComponent>(registry, entity)) {
            *existing = value;
        } else {
            ecs::emplace<ObjectActiveShroudComponent>(registry, entity,
                                                      value);
        }
        return;
    }
    case game::ObjectUpgradeOperation::Radar:
    {
        ObjectRadarProviderComponent* provider =
            ecs::try_get<ObjectRadarProviderComponent>(registry, entity);
        if (!provider)
            provider = &ecs::emplace<ObjectRadarProviderComponent>(registry,
                                                                    entity);
        if (provider->providerCount != std::numeric_limits<uint32_t>::max())
            ++provider->providerCount;
        if (rule.radarDisableProof &&
            provider->disableProofProviderCount !=
                std::numeric_limits<uint32_t>::max())
        {
            ++provider->disableProofProviderCount;
        }
        provider->activatedTick = confirmedTick;
        if (ObjectRadarUpdateComponent* radar =
                ecs::try_get<ObjectRadarUpdateComponent>(registry, entity)) {
            beginRadarExtension(registry, entity, *radar, rules,
                                confirmedTick);
        }
        return;
    }
    case game::ObjectUpgradeOperation::PassengersFire:
    {
        ObjectContainmentComponent* containment =
            ecs::try_get<ObjectContainmentComponent>(registry, entity);
        // RefCode asks Object::getContain() and becomes a sticky no-op when
        // the host has no ContainModuleInterface.  Do not manufacture a
        // containment host merely because malformed/modded content attached
        // PassengersFireUpgrade to an unrelated object.
        if (!containment) return;
        if (!containment->passengersAllowedToFire) {
            containment->passengersAllowedToFire = true;
            if (containment->revision !=
                std::numeric_limits<uint64_t>::max()) {
                ++containment->revision;
            }
        }
        return;
    }
    case game::ObjectUpgradeOperation::UnpauseSpecialPower:
    {
        ObjectSpecialPowerComponent* powers =
            ecs::try_get<ObjectSpecialPowerComponent>(registry, entity);
        if (!powers || !powers->plan) return;
        const size_t count = std::min(powers->instances.size(),
                                      powers->plan->rules.size());
        for (size_t index = 0; index < count; ++index)
        {
            if (!asciiEqualIgnoreCase(
                    powers->plan->rules[index].specialPowerTemplate,
                    rule.specialPowerTemplate))
                continue;
            ObjectSpecialPowerRuntime& runtime = powers->instances[index];
            if (runtime.pausedCount == 0) continue;
            --runtime.pausedCount;
            if (runtime.pausedCount != 0) continue;

            // SpecialPowerModule::pauseCountdown(FALSE) credits no recharge
            // time while paused: once the final pause source is removed, the
            // ready deadline moves forward by the frozen interval.
            const uint64_t pausedTicks =
                confirmedTick >= runtime.pauseStartedTick
                    ? confirmedTick - runtime.pauseStartedTick : 0;
            const SpecialPowerDefinition* definition = context.content
                ? context.content->findSpecialPower(runtime.content) : nullptr;
            // SharedNSync readiness belongs to the controlling player's one
            // shared clock in RefCode. A locally paused provider rejoins that
            // clock without pushing every peer's deadline forward.
            if ((!definition || !definition->sharedSyncedTimer) &&
                runtime.readyTick != std::numeric_limits<uint64_t>::max()) {
                runtime.readyTick = saturatingAdd(runtime.readyTick,
                                                  pausedTicks);
            }
            runtime.pauseStartedTick = 0;
        }
        return;
    }
    }
}

} // namespace object_upgrade_detail
} // namespace engine
