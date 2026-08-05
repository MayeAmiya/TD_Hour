#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectDirty.h"

#include "core/container/string_utils.h"
#include <algorithm>
#include <optional>

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/structure/ObjectMissileLauncherBuilding.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

namespace engine
{
namespace
{

struct ConditionCatalog final
{
    game::ModelConditionMask owned;
    game::ModelConditionMask moving;
    game::ModelConditionMask overWater;
    game::ModelConditionMask attacking;
    game::ModelConditionMask dying;
    game::ModelConditionMask awaitingConstruction;
    game::ModelConditionMask partiallyConstructed;
    game::ModelConditionMask activelyBeingConstructed;
    game::ModelConditionMask activelyConstructing;
    game::ModelConditionMask sold;
    game::ModelConditionMask garrisoned;
    game::ModelConditionMask constructionComplete;
    game::ModelConditionMask night;
    game::ModelConditionMask snow;
    game::ModelConditionMask rappelling;
    game::ModelConditionMask particleUplinkOwned;
    game::ModelConditionMask packing;
    game::ModelConditionMask unpacking;
    game::ModelConditionMask deployed;
    game::ModelConditionMask postCollapse;
    game::ModelConditionMask freefall;
    game::ModelConditionMask continuousFireMean;
    game::ModelConditionMask continuousFireFast;
    game::ModelConditionMask continuousFireSlow;
    game::ModelConditionMask turretRotate;
    container::Array<game::ModelConditionMask, game::kWeaponSlotCount> weaponOwned;
    container::Array<game::ModelConditionMask, game::kWeaponSlotCount> preattack;
    container::Array<game::ModelConditionMask, game::kWeaponSlotCount> firing;
    container::Array<game::ModelConditionMask, game::kWeaponSlotCount> between;
    container::Array<game::ModelConditionMask, game::kWeaponSlotCount> reloading;
    container::Array<game::ModelConditionMask, game::kWeaponSlotCount> usingWeapon;
    container::Array<game::ModelConditionMask, 4> doorOwned;
    container::Array<game::ModelConditionMask, 4> doorOpening;
    container::Array<game::ModelConditionMask, 4> doorClosing;
    container::Array<game::ModelConditionMask, 4> doorWaitingOpen;
    container::Array<game::ModelConditionMask, 4> doorWaitingToClose;
};

void addMask(game::ModelConditionMask& target, const game::ModelConditionMask& source) noexcept
{
    target.words[0] |= source.words[0];
    target.words[1] |= source.words[1];
}

[[nodiscard]] const ConditionCatalog& conditions();

void applyDoorPhase(game::ModelConditionMask& selected,
                    const ConditionCatalog& catalog,
                    size_t slot,
                    ObjectModelConditionDoorPhase phase) noexcept
{
    if (slot >= catalog.doorOwned.size()) return;
    selected.clear(catalog.doorOwned[slot]);
    switch (phase)
    {
    case ObjectModelConditionDoorPhase::Opening:
        addMask(selected, catalog.doorOpening[slot]);
        break;
    case ObjectModelConditionDoorPhase::WaitingOpen:
        addMask(selected, catalog.doorWaitingOpen[slot]);
        break;
    case ObjectModelConditionDoorPhase::WaitingToClose:
        addMask(selected, catalog.doorWaitingToClose[slot]);
        break;
    case ObjectModelConditionDoorPhase::Closing:
        addMask(selected, catalog.doorClosing[slot]);
        break;
    case ObjectModelConditionDoorPhase::Unspecified:
        break;
    }
}

void appendStagedDoorConditions(
    game::ModelConditionMask& selected,
    const ObjectModelConditionDoorContributionComponent* contributions) noexcept
{
    if (!contributions) return;
    const ConditionCatalog& catalog = conditions();
    for (size_t slot = 0; slot < ObjectModelConditionDoorContributionComponent::kDoorSlotCount; ++slot)
    {
        bool found = false;
        ObjectModelConditionDoorPhase phase = ObjectModelConditionDoorPhase::Unspecified;
        // Later entries deliberately win. This is the sealed legacy producer
        // order and is independent of ECS registration order.
        for (size_t source = 0; source < ObjectModelConditionDoorContributionComponent::kSourceCount; ++source)
        {
            const auto candidate = contributions->phases[source][slot];
            if (candidate == ObjectModelConditionDoorPhase::Unspecified) continue;
            found = true;
            phase = candidate;
        }
        if (found) applyDoorPhase(selected, catalog, slot, phase);
    }
}

[[nodiscard]] const ConditionCatalog& conditions()
{
    static const ConditionCatalog result = []
    {
        ConditionCatalog value;
        value.moving = game::modelConditionMaskOf(game::ModelConditionFlag::Moving);
        value.overWater = game::modelConditionMaskOf(game::ModelConditionFlag::OverWater);
        value.attacking = game::modelConditionMaskOf(game::ModelConditionFlag::Attacking);
        value.dying = game::modelConditionMaskOf(game::ModelConditionFlag::Dying);
        value.awaitingConstruction = game::modelConditionMaskOf(game::ModelConditionFlag::AwaitingConstruction);
        value.partiallyConstructed = game::modelConditionMaskOf(game::ModelConditionFlag::PartiallyConstructed);
        value.activelyBeingConstructed = game::modelConditionMaskOf(game::ModelConditionFlag::ActivelyBeingConstructed);
        value.activelyConstructing = game::modelConditionMaskOf(game::ModelConditionFlag::ActivelyConstructing);
        value.sold = game::modelConditionMaskOf(game::ModelConditionFlag::Sold);
        value.garrisoned = game::modelConditionMaskOf(game::ModelConditionFlag::Garrisoned);
        value.constructionComplete = game::modelConditionMaskOf(game::ModelConditionFlag::ConstructionComplete);
        value.night = game::modelConditionMaskOf(game::ModelConditionFlag::Night);
        value.snow = game::modelConditionMaskOf(game::ModelConditionFlag::Snow);
        value.rappelling = game::modelConditionMaskOf(game::ModelConditionFlag::Rappelling);
        value.packing = game::modelConditionMaskOf(game::ModelConditionFlag::Packing);
        value.unpacking = game::modelConditionMaskOf(game::ModelConditionFlag::Unpacking);
        value.deployed = game::modelConditionMaskOf(game::ModelConditionFlag::Deployed);
        value.postCollapse = game::modelConditionMaskOf(game::ModelConditionFlag::PostCollapse);
        value.freefall = game::modelConditionMaskOf(game::ModelConditionFlag::FreeFall);
        value.continuousFireMean =
            game::modelConditionMaskOf(game::ModelConditionFlag::ContinuousFireMean);
        value.continuousFireFast =
            game::modelConditionMaskOf(game::ModelConditionFlag::ContinuousFireFast);
        value.continuousFireSlow =
            game::modelConditionMaskOf(game::ModelConditionFlag::ContinuousFireSlow);
        value.turretRotate =
            game::modelConditionMaskOf(game::ModelConditionFlag::TurretRotate);
        value.particleUplinkOwned = game::modelConditionMaskOf(
            game::ModelConditionFlag::Packing,
            game::ModelConditionFlag::Unpacking,
            game::ModelConditionFlag::Deployed);

        constexpr container::Array<game::ModelConditionFlag, game::kWeaponSlotCount>
            preattackFlags{
                game::ModelConditionFlag::PreattackA,
                game::ModelConditionFlag::PreattackB,
                game::ModelConditionFlag::PreattackC};
        constexpr container::Array<game::ModelConditionFlag, game::kWeaponSlotCount>
            firingFlags{
                game::ModelConditionFlag::FiringA,
                game::ModelConditionFlag::FiringB,
                game::ModelConditionFlag::FiringC};
        constexpr container::Array<game::ModelConditionFlag, game::kWeaponSlotCount>
            betweenFlags{
                game::ModelConditionFlag::BetweenFiringShotsA,
                game::ModelConditionFlag::BetweenFiringShotsB,
                game::ModelConditionFlag::BetweenFiringShotsC};
        constexpr container::Array<game::ModelConditionFlag, game::kWeaponSlotCount>
            reloadingFlags{
                game::ModelConditionFlag::ReloadingA,
                game::ModelConditionFlag::ReloadingB,
                game::ModelConditionFlag::ReloadingC};
        constexpr container::Array<game::ModelConditionFlag, game::kWeaponSlotCount>
            usingWeaponFlags{
                game::ModelConditionFlag::UsingWeaponA,
                game::ModelConditionFlag::UsingWeaponB,
                game::ModelConditionFlag::UsingWeaponC};
        for (size_t index = 0; index < game::kWeaponSlotCount; ++index)
        {
            value.preattack[index] = game::modelConditionMaskOf(preattackFlags[index]);
            value.firing[index] = game::modelConditionMaskOf(firingFlags[index]);
            value.between[index] = game::modelConditionMaskOf(betweenFlags[index]);
            value.reloading[index] = game::modelConditionMaskOf(reloadingFlags[index]);
            value.usingWeapon[index] = game::modelConditionMaskOf(usingWeaponFlags[index]);
            addMask(value.weaponOwned[index], value.preattack[index]);
            addMask(value.weaponOwned[index], value.firing[index]);
            addMask(value.weaponOwned[index], value.between[index]);
            addMask(value.weaponOwned[index], value.reloading[index]);
            addMask(value.weaponOwned[index], value.usingWeapon[index]);
            addMask(value.owned, value.weaponOwned[index]);
        }

        constexpr container::Array<game::ModelConditionFlag, 4> doorOpeningFlags{
            game::ModelConditionFlag::Door1Opening,
            game::ModelConditionFlag::Door2Opening,
            game::ModelConditionFlag::Door3Opening,
            game::ModelConditionFlag::Door4Opening};
        constexpr container::Array<game::ModelConditionFlag, 4> doorClosingFlags{
            game::ModelConditionFlag::Door1Closing,
            game::ModelConditionFlag::Door2Closing,
            game::ModelConditionFlag::Door3Closing,
            game::ModelConditionFlag::Door4Closing};
        constexpr container::Array<game::ModelConditionFlag, 4> doorWaitingOpenFlags{
            game::ModelConditionFlag::Door1WaitingOpen,
            game::ModelConditionFlag::Door2WaitingOpen,
            game::ModelConditionFlag::Door3WaitingOpen,
            game::ModelConditionFlag::Door4WaitingOpen};
        constexpr container::Array<game::ModelConditionFlag, 4> doorWaitingToCloseFlags{
            game::ModelConditionFlag::Door1WaitingToClose,
            game::ModelConditionFlag::Door2WaitingToClose,
            game::ModelConditionFlag::Door3WaitingToClose,
            game::ModelConditionFlag::Door4WaitingToClose};
        for (size_t index = 0; index < value.doorOwned.size(); ++index)
        {
            value.doorOpening[index] =
                game::modelConditionMaskOf(doorOpeningFlags[index]);
            value.doorClosing[index] =
                game::modelConditionMaskOf(doorClosingFlags[index]);
            value.doorWaitingOpen[index] =
                game::modelConditionMaskOf(doorWaitingOpenFlags[index]);
            value.doorWaitingToClose[index] =
                game::modelConditionMaskOf(doorWaitingToCloseFlags[index]);
            // WAITING_TO_CLOSE is part of the producer-owned family even
            // though ProductionUpdate never selects it in stock RefCode.
            value.doorOwned[index] = game::modelConditionMaskOf(
                doorOpeningFlags[index],
                doorClosingFlags[index],
                doorWaitingOpenFlags[index],
                doorWaitingToCloseFlags[index]);
            addMask(value.owned, value.doorOwned[index]);
        }

        addMask(value.owned, value.moving);
        addMask(value.owned, value.overWater);
        addMask(value.owned, value.attacking);
        addMask(value.owned, value.dying);
        addMask(value.owned, value.awaitingConstruction);
        addMask(value.owned, value.partiallyConstructed);
        addMask(value.owned, value.activelyBeingConstructed);
        addMask(value.owned, value.activelyConstructing);
        addMask(value.owned, value.sold);
        addMask(value.owned, value.garrisoned);
        addMask(value.owned, value.constructionComplete);
        addMask(value.owned, value.night);
        addMask(value.owned, value.snow);
        addMask(value.owned, value.rappelling);
        addMask(value.owned, value.continuousFireMean);
        addMask(value.owned, value.continuousFireFast);
        addMask(value.owned, value.continuousFireSlow);
        addMask(value.owned, value.turretRotate);
        return value;
    }();
    return result;
}

[[nodiscard]] bool hasGarrisonContainRecipe(const ecs::registry& registry, ecs::entity entity) noexcept
{
    const ThingTemplateComponent* type = ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->containmentPlan)
        return false;
    return std::any_of(
        type->archetype->containmentPlan->rules.begin(),
        type->archetype->containmentPlan->rules.end(),
        [](const ObjectContainmentRule& rule) noexcept {
            return rule.kind == ObjectContainmentKind::Garrison;
        });
}

[[nodiscard]] bool garrisonConditionVisible(const ecs::registry& registry,
                                            const ObjectLifecycle& lifecycle,
                                            ecs::entity host,
                                            const ObjectContainmentComponent& containment,
                                            const ObjectModelConditionEnvironment&
                                                environment) noexcept
{
    if (containment.objects.empty() || !hasGarrisonContainRecipe(registry, host))
    {
        return false;
    }
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, host);
    const bool locallyOwned = owner && environment.localPlayer &&
        owner->player == environment.localPlayer;

    const game::ObjectStatusMask detected = game::objectStatusBit(game::ObjectStatusFlag::Detected);
    const game::ObjectStatusMask stealthed = game::objectStatusBit(game::ObjectStatusFlag::Stealthed);
    bool hasLiveOccupant = false;
    bool allOccupantsStealthed = true;
    PlayerId controllingPlayer = INVALID_PLAYER_ID;
    for (const ObjectContainedObjectRecord& record : containment.objects)
    {
        // DestroyRequested containment edges are cleared at the structural
        // frame boundary, after this authority runs. Do not freeze a stale
        // one-frame GARRISONED condition for an occupant that is already
        // logically unavailable.
        const std::optional<ecs::entity> occupant = lifecycle.entityFromId(record.object);
        if (!occupant)
            continue;
        hasLiveOccupant = true;
        if (!controllingPlayer) {
            if (const OwnerComponent* occupantOwner =
                    ecs::try_get<OwnerComponent>(registry, *occupant)) {
                controllingPlayer = occupantOwner->player;
            }
        }
        const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, *occupant);
        if (status && status->hasAny(detected))
            return true;
        if (!status || !status->hasAny(stealthed))
            allOccupantsStealthed = false;
    }
    if (!hasLiveOccupant)
        return false;
    // RefCode hides GARRISONED only when every live occupant is stealthy and
    // the observing player is not allied with the new controller.  A normal
    // non-stealth garrison is public even though it never carries DETECTED.
    if (!allOccupantsStealthed || !environment.localPlayer)
        return true;
    if (!controllingPlayer)
        return locallyOwned;
    if (!environment.players)
        return controllingPlayer == environment.localPlayer;
    return environment.players->relationship(
               controllingPlayer, environment.localPlayer) ==
        PlayerRelationship::Allies;
}

[[nodiscard]] bool isAttacking(const ObjectWeaponComponent& weapons, const ObjectStatusComponent* status) noexcept
{
    const bool statusAttacking = status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::IsAttacking));
    return statusAttacking || (weapons.state != ObjectWeaponRuntimeState::Idle &&
                               weapons.state != ObjectWeaponRuntimeState::NoUsableWeapon);
}

void appendWeaponConditions(game::ModelConditionMask& selected,
                            const ObjectWeaponComponent& weapons,
                            const ObjectStatusComponent* status,
                            uint64_t confirmedTick) noexcept
{
    const ConditionCatalog& catalog = conditions();
    switch (weapons.continuousFireStage) {
    case ObjectContinuousFireStage::Mean:
        addMask(selected, catalog.continuousFireMean);
        break;
    case ObjectContinuousFireStage::Fast:
        addMask(selected, catalog.continuousFireFast);
        break;
    case ObjectContinuousFireStage::Slow:
        addMask(selected, catalog.continuousFireSlow);
        break;
    case ObjectContinuousFireStage::None:
        break;
    }
    if (!weapons.currentSlot || !weapons.activeWeaponSetIndex || *weapons.activeWeaponSetIndex >= weapons.sets.size())
    {
        return;
    }
    const size_t slotIndex = static_cast<size_t>(*weapons.currentSlot);
    if (slotIndex >= game::kWeaponSlotCount)
        return;
    const ObjectWeaponSetRuntime& set = weapons.sets[*weapons.activeWeaponSetIndex];
    const ObjectWeaponSlotRuntime& slot = set.slots[slotIndex];
    const bool attacking = isAttacking(weapons, status);

    const game::ModelConditionMask* phase = nullptr;
    if (slot.lastFireSequence != 0 && slot.lastFireTick == confirmedTick)
    {
        phase = &catalog.firing[slotIndex];
    }
    else if (!attacking)
    {
        return;
    }
    else if (weapons.state == ObjectWeaponRuntimeState::Reloading ||
             (set.sharedReloadCompleteTick != 0 && confirmedTick < set.sharedReloadCompleteTick) ||
             (slot.reloadCompleteTick != 0 && confirmedTick < slot.reloadCompleteTick))
    {
        phase = &catalog.reloading[slotIndex];
    }
    else if (weapons.state == ObjectWeaponRuntimeState::WindingUp ||
             (slot.preAttackArmed && confirmedTick < slot.preAttackCompleteTick))
    {
        phase = &catalog.preattack[slotIndex];
    }
    else if (weapons.state == ObjectWeaponRuntimeState::CoolingDown)
    {
        phase = &catalog.between[slotIndex];
    }
    else if (weapons.state == ObjectWeaponRuntimeState::TrackingTarget &&
             status && status->hasAny(
                 game::objectStatusBit(
                     game::ObjectStatusFlag::IsAimingWeapon) |
                 game::objectStatusBit(
                     game::ObjectStatusFlag::IsFiringWeapon)))
    {
        // ZH keeps the BETWEEN pose for the one-frame READY_TO_FIRE gap in a
        // burst while the object is still aiming/firing. Falling back to the
        // idle condition here makes launchers and burst weapons visibly pop
        // to their default model between authored shots.
        phase = &catalog.between[slotIndex];
    }

    if (phase)
    {
        addMask(selected, *phase);
        addMask(selected, catalog.usingWeapon[slotIndex]);
    }
}

void appendProductionConditions(game::ModelConditionMask& selected,
                                const ObjectProductionComponent* production,
                                uint64_t confirmedTick) noexcept
{
    if (!production || !production->plan)
        return;
    const ConditionCatalog& catalog = conditions();
    if (production->constructionCompleteActive && confirmedTick >= production->constructionCompleteVisibleTick)
    {
        addMask(selected, catalog.constructionComplete);
    }
    const size_t doorCount = std::min<size_t>(production->plan->numberOfDoorAnimations, production->doors.size());
    for (size_t index = 0; index < doorCount; ++index)
    {
        const ObjectProductionDoorRuntime& door = production->doors[index];
        const ObjectProductionDoorPhase visiblePhase =
            confirmedTick < door.conditionVisibleTick ? door.previousVisiblePhase : door.phase;
        switch (visiblePhase)
        {
        case ObjectProductionDoorPhase::Opening:
            addMask(selected, catalog.doorOpening[index]);
            break;
        case ObjectProductionDoorPhase::WaitingOpen:
            addMask(selected, catalog.doorWaitingOpen[index]);
            break;
        case ObjectProductionDoorPhase::Closing:
            addMask(selected, catalog.doorClosing[index]);
            break;
        case ObjectProductionDoorPhase::Closed:
            break;
        }
    }
}

void appendMissileLauncherConditions(
    game::ModelConditionMask& selected,
    const ObjectMissileLauncherBuildingComponent* launcher) noexcept {
    if (!launcher || !launcher->plan) return;
    const ConditionCatalog& catalog = conditions();
    const size_t count = std::min(
        launcher->plan->rules.size(), launcher->instances.size());
    for (size_t index = 0; index < count; ++index) {
        // MissileLauncherBuildingUpdate owns door 1 independently from the
        // production queue. A later authored occurrence replaces the same
        // legacy clearAndSet family rather than accumulating impossible door
        // phases.
        selected.clear(catalog.doorOwned[0]);
        switch (launcher->instances[index].phase) {
        case ObjectMissileLauncherDoorPhase::Opening:
            addMask(selected, catalog.doorOpening[0]);
            break;
        case ObjectMissileLauncherDoorPhase::Open:
            addMask(selected, catalog.doorWaitingOpen[0]);
            break;
        case ObjectMissileLauncherDoorPhase::WaitingToClose:
            addMask(selected, catalog.doorWaitingToClose[0]);
            break;
        case ObjectMissileLauncherDoorPhase::Closing:
            addMask(selected, catalog.doorClosing[0]);
            break;
        case ObjectMissileLauncherDoorPhase::Closed:
            break;
        }
    }
}

void appendParticleUplinkConditions(
    game::ModelConditionMask& selected,
    const ObjectParticleUplinkComponent* uplink) noexcept {
    if (!uplink || !uplink->plan) return;
    const ConditionCatalog& catalog = conditions();
    const size_t count = std::min(
        uplink->plan->rules.size(), uplink->instances.size());
    for (size_t index = 0; index < count; ++index) {
        selected.clear(catalog.particleUplinkOwned);
        switch (uplink->instances[index].phase) {
        case ObjectParticleUplinkPhase::Preparing:
            addMask(selected, catalog.unpacking);
            break;
        case ObjectParticleUplinkPhase::AlmostReady:
        case ObjectParticleUplinkPhase::ReadyToFire:
        case ObjectParticleUplinkPhase::Prefire:
        case ObjectParticleUplinkPhase::Firing:
        case ObjectParticleUplinkPhase::Postfire:
            addMask(selected, catalog.deployed);
            break;
        case ObjectParticleUplinkPhase::Packing:
            addMask(selected, catalog.packing);
            break;
        case ObjectParticleUplinkPhase::Idle:
        case ObjectParticleUplinkPhase::Charging:
            break;
        }
    }
}

} // namespace

void publishObjectModelConditionDoor(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectModelConditionDoorSource source,
    size_t doorSlot,
    ObjectModelConditionDoorPhase phase,
    uint64_t confirmedTick,
    uint64_t sequence) noexcept
{
    const size_t sourceIndex = static_cast<size_t>(source);
    if (sourceIndex >= ObjectModelConditionDoorContributionComponent::kSourceCount ||
        doorSlot >= ObjectModelConditionDoorContributionComponent::kDoorSlotCount)
    {
        return;
    }
    ObjectModelConditionDoorContributionComponent* component =
        ecs::try_get<ObjectModelConditionDoorContributionComponent>(registry, entity);
    if (!component)
    {
        component = &ecs::emplace<ObjectModelConditionDoorContributionComponent>(registry, entity);
    }
    const uint64_t previousTick = component->confirmedTicks[sourceIndex][doorSlot];
    const uint64_t previousSequence = component->sequences[sourceIndex][doorSlot];
    if (confirmedTick < previousTick ||
        (confirmedTick == previousTick && sequence < previousSequence))
    {
        return;
    }
    component->phases[sourceIndex][doorSlot] = phase;
    component->confirmedTicks[sourceIndex][doorSlot] = confirmedTick;
    component->sequences[sourceIndex][doorSlot] = sequence;
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
}

void publishObjectModelConditionContribution(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectModelConditionContributionSource source,
    const game::ModelConditionMask& clearMask,
    const game::ModelConditionMask& setMask,
    uint64_t confirmedTick,
    uint64_t sequence) noexcept
{
    const size_t sourceIndex = static_cast<size_t>(source);
    if (sourceIndex >= ObjectModelConditionContributionComponent::kSourceCount)
        return;
    ObjectModelConditionContributionComponent* component =
        ecs::try_get<ObjectModelConditionContributionComponent>(registry, entity);
    if (!component)
        component = &ecs::emplace<ObjectModelConditionContributionComponent>(registry, entity);
    if (confirmedTick < component->confirmedTicks[sourceIndex] ||
        (confirmedTick == component->confirmedTicks[sourceIndex] &&
         sequence < component->sequences[sourceIndex]))
        return;

    game::ModelConditionMask family = clearMask;
    addMask(family, setMask);
    addMask(component->owned[sourceIndex], family);
    component->selected[sourceIndex].clear(clearMask);
    addMask(component->selected[sourceIndex], setMask);
    component->confirmedTicks[sourceIndex] = confirmedTick;
    component->sequences[sourceIndex] = sequence;

    // A few confirmed gameplay entry points (notably the public upgrade
    // completion transaction) expose their newly selected visual state
    // synchronously before the end-of-tick compose. Preserve that observable
    // RefCode behavior without allowing a producer to edit RenderModel
    // directly. This eager projection only adds bits: source withdrawal and
    // mutually-owned clearing remain exclusively in the full authority pass,
    // where every core and contributed source is available.
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity)) {
        addMask(visual->modelConditionFlags, setMask);
    }
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
}

ObjectModelConditionAuthorityReport updateAuthoritativeObjectModelConditions(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectModelConditionAuthorityState& state,
    const ObjectModelConditionEnvironment& environment,
    uint64_t confirmedTick)
{
    struct Candidate final
    {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const bool environmentChanged = !state.initialized ||
        state.localPlayer != environment.localPlayer ||
        state.forceModelsToFollowTimeOfDay !=
            environment.forceModelsToFollowTimeOfDay ||
        state.forceModelsToFollowWeather !=
            environment.forceModelsToFollowWeather ||
        state.night != environment.night || state.snowy != environment.snowy;
    state = {
        .localPlayer = environment.localPlayer,
        .forceModelsToFollowTimeOfDay =
            environment.forceModelsToFollowTimeOfDay,
        .forceModelsToFollowWeather =
            environment.forceModelsToFollowWeather,
        .night = environment.night,
        .snowy = environment.snowy,
        .initialized = true,
    };
    // A model-condition mark may outlive its visual or lifecycle record when
    // destruction removes components between producer and finalization.
    // Retaining it would make the sparse dirty view revisit a tombstone every
    // confirmed tick. A later RenderModel attachment is responsible for
    // publishing a fresh mark at its own creation boundary.
    container::Vector<ecs::entity> staleDirty;
    const auto dirtyView = ecs::view<const ObjectIdentityComponent,
                                     ObjectDirtyComponent>(registry);
    for (const ecs::entity entity : dirtyView) {
        const ObjectDirtyComponent& dirty =
            dirtyView.template get<ObjectDirtyComponent>(entity);
        if ((dirty.domains & objectDirtyBit(
                ObjectDirtyDomain::ModelCondition)) == 0) {
            continue;
        }
        const ObjectIdentityComponent& identity =
            dirtyView.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id ||
            !ecs::try_get<RenderModelComponent>(registry, entity) ||
            !lifecycle.entityFromIdIncludingPending(identity.id)) {
            staleDirty.push_back(entity);
        }
    }
    for (const ecs::entity entity : staleDirty) {
        clearObjectDirty(registry, entity,
                         ObjectDirtyDomain::ModelCondition);
    }
    if (environmentChanged) {
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    RenderModelComponent>(registry);
        candidates.reserve(view.size_hint());
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            if (identity.id &&
                lifecycle.entityFromIdIncludingPending(identity.id)) {
                candidates.push_back({identity.id, entity});
            }
        }
    } else {
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    ObjectDirtyComponent>(registry);
        for (const ecs::entity entity : view) {
            const ObjectDirtyComponent& dirty =
                view.template get<ObjectDirtyComponent>(entity);
            if ((dirty.domains & objectDirtyBit(
                    ObjectDirtyDomain::ModelCondition)) == 0 ||
                !ecs::try_get<RenderModelComponent>(registry, entity)) {
                continue;
            }
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            if (identity.id &&
                lifecycle.entityFromIdIncludingPending(identity.id)) {
                candidates.push_back({identity.id, entity});
            }
        }
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });

    ObjectModelConditionAuthorityReport report;
    const ConditionCatalog& catalog = conditions();
    for (const Candidate& candidate : candidates)
    {
        ++report.visitedObjects;
        RenderModelComponent& visual = ecs::get<RenderModelComponent>(registry, candidate.entity);
        game::ModelConditionMask selected;

        const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        if (health) {
            // BodyDamageType remains a logical HP-derived state during
            // construction, while Drawable stays on construction art. Keep
            // that invariant at the final model-condition authority too, so
            // no independent producer can leak DAMAGED/REALLY_DAMAGED bits.
            projectObjectBodyDamageVisual(
                registry, candidate.entity, health->damageState, visual);
        }
        // Bodyless/InactiveBody recipes are represented by a non-damageable
        // health shell and may report effectivelyDead for gameplay queries;
        // they never enter RefCode's AI dead state and must not select DYING.
        const bool dying = health && health->acceptsDamage && health->effectivelyDead;
        if (dying)
            addMask(selected, catalog.dying);

        const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        if (status)
        {
            if (status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction)))
            {
                const ObjectConstructionSiteComponent* site =
                    ecs::try_get<ObjectConstructionSiteComponent>(registry,
                                                                  candidate.entity);
                if (site && site->completedFrames != 0) {
                    addMask(selected, catalog.partiallyConstructed);
                    if (site->builder)
                        addMask(selected, catalog.activelyBeingConstructed);
                } else {
                    addMask(selected, catalog.awaitingConstruction);
                }
            }
            if (status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Sold)))
            {
                const ObjectSaleComponent* sale =
                    ecs::try_get<ObjectSaleComponent>(registry, candidate.entity);
                // RefCode publishes the construction pair as soon as selling
                // begins, while the object remains at 99.9% for the initial
                // scaffold-rise window. It then reverses construction toward
                // zero. SOLD replaces the pair only when the descent crosses
                // zero; the final 0..-50 interval lets the scaffold finish
                // below ground before lifecycle retirement.
                if (!sale || sale->soldVisualActive(confirmedTick)) {
                    addMask(selected, catalog.sold);
                } else {
                    addMask(selected, catalog.partiallyConstructed);
                    addMask(selected, catalog.activelyBeingConstructed);
                }
            }
        }

        if (!dying)
        {
            if (const ObjectBuilderComponent* builder =
                    ecs::try_get<ObjectBuilderComponent>(registry,
                                                         candidate.entity)) {
                const bool constructing = std::any_of(
                    builder->runtimes.begin(), builder->runtimes.end(),
                    [](const ObjectBuilderRuntime& runtime) {
                        // RefCode borrows ACTIVELY_CONSTRUCTING while a
                        // Dozer/Worker is actually welding a repair target.
                        return runtime.phase == ObjectBuilderPhase::Building ||
                            runtime.phase == ObjectBuilderPhase::Repairing;
                    });
                if (constructing)
                    addMask(selected, catalog.activelyConstructing);
            }
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry, candidate.entity);
            if (locomotion && locomotion->hasActiveMove && locomotion->state != ObjectLocomotionState::Idle)
            {
                addMask(selected, catalog.moving);
            }
            if (locomotion && locomotion->overWater)
            {
                addMask(selected, catalog.overWater);
            }
            if (const ObjectSpawnSlaveComponent* spawnSlave =
                    ecs::try_get<ObjectSpawnSlaveComponent>(
                        registry, candidate.entity)) {
                for (const ObjectTensileRuntime& tensile :
                     spawnSlave->tensileFormations) {
                    if (!tensile.activationPublished || tensile.terminal)
                        continue;
                    addMask(selected, catalog.postCollapse);
                    if (tensile.movingCondition)
                        addMask(selected, catalog.moving);
                    if (tensile.freefallCondition)
                        addMask(selected, catalog.freefall);
                }
            }
            const ObjectWeaponComponent* weapons = ecs::try_get<ObjectWeaponComponent>(registry, candidate.entity);
            bool attacking = status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::IsAttacking));
            if (weapons)
            {
                attacking = attacking || isAttacking(*weapons, status);
                appendWeaponConditions(selected, *weapons, status, confirmedTick);
                // TurretAI::friend_turnTowardsAngle sets TURRET_ROTATE on the
                // owning Object while a turret is still stepping toward its
                // desired angle (TurretAI.cpp:394 clears, :403 sets). RefCode's
                // single object-level bit lets a second TurretAI overwrite the
                // first; unioning the per-turret facts keeps every authored
                // TURRET_ROTATE state reachable and stays deterministic.
                if (std::any_of(weapons->turrets.begin(), weapons->turrets.end(),
                                [](const ObjectTurretRuntime& turret) {
                                    return turret.controlledWeaponSlots != 0 &&
                                        turret.rotating;
                                }))
                {
                    addMask(selected, catalog.turretRotate);
                }
                const ObjectSpawnSlaveComponent* spawnSlave =
                    ecs::try_get<ObjectSpawnSlaveComponent>(
                        registry, candidate.entity);
                static const game::ModelConditionMask playerUpgrade =
                    game::modelConditionMaskOf(game::ModelConditionFlag::WeaponsetPlayerUpgrade);
                if (spawnSlave && !spawnSlave->mobMemberSlaved.empty() &&
                    visual.modelConditionFlags.intersectionCount(
                        playerUpgrade) != 0u) {
                    // MobMemberSlavedUpdate suppresses the entire weapon-A
                    // animation family while the upgraded weapon set owns
                    // the drawable. Do this after the normal weapon producer
                    // so the authority cannot re-add bits cleared by Mob.
                    selected.clear(catalog.weaponOwned[0]);
                }
            }
            if (const ObjectSpawnSlaveComponent* spawnSlave =
                    ecs::try_get<ObjectSpawnSlaveComponent>(
                        registry, candidate.entity)) {
                for (const ObjectSlaveRuntime& slave :
                     spawnSlave->slaved) {
                    if (slave.repairState ==
                            ObjectSlaveRepairState::Extending ||
                        slave.repairState ==
                            ObjectSlaveRepairState::Welding) {
                        addMask(selected,
                                catalog.firing[1]);
                    } else if (slave.repairState ==
                               ObjectSlaveRepairState::Retracting) {
                        addMask(selected,
                                catalog.firing[2]);
                    }
                }
            }
            if (attacking)
            {
                addMask(selected, catalog.attacking);
            }
        }

        appendProductionConditions(
            selected, ecs::try_get<ObjectProductionComponent>(registry, candidate.entity), confirmedTick);
        appendStagedDoorConditions(
            selected,
            ecs::try_get<ObjectModelConditionDoorContributionComponent>(
                registry, candidate.entity));
        if (!(status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction)))) {
            // MissileLauncherBuildingUpdate owns Door1's complete phase
            // machine. Apply it after generic staged contributors so an old
            // SpecialPower/Containment contribution cannot replace the
            // WaitingToClose -> Closing generation that plays the authored
            // launch/return clip.
            appendMissileLauncherConditions(
                selected,
                ecs::try_get<ObjectMissileLauncherBuildingComponent>(
                    registry, candidate.entity));
            appendParticleUplinkConditions(
                selected,
                ecs::try_get<ObjectParticleUplinkComponent>(
                    registry, candidate.entity));
        }

        const ObjectContainmentComponent* containment =
            ecs::try_get<ObjectContainmentComponent>(registry, candidate.entity);
        if (containment &&
            garrisonConditionVisible(registry, lifecycle, candidate.entity,
                                     *containment, environment))
        {
            addMask(selected, catalog.garrisoned);
        }
        const ObjectEnvironmentModelConditionOverrideComponent*
            mapEnvironment = ecs::try_get<
                ObjectEnvironmentModelConditionOverrideComponent>(
                registry, candidate.entity);
        const bool objectNight = mapEnvironment && mapEnvironment->night >= 0
            ? mapEnvironment->night != 0
            : environment.night;
        const bool objectSnow = mapEnvironment && mapEnvironment->snow >= 0
            ? mapEnvironment->snow != 0
            : environment.snowy;
        const bool hasObjectNight =
            mapEnvironment && mapEnvironment->night >= 0;
        const bool hasObjectSnow =
            mapEnvironment && mapEnvironment->snow >= 0;
        if ((hasObjectNight ||
             environment.forceModelsToFollowTimeOfDay) && objectNight)
        {
            addMask(selected, catalog.night);
        }
        if ((hasObjectSnow ||
             environment.forceModelsToFollowWeather) && objectSnow)
        {
            addMask(selected, catalog.snow);
        }
        if (ecs::try_get<ObjectRappellingComponent>(
                registry, candidate.entity))
        {
            addMask(selected, catalog.rappelling);
        }

        const ObjectModelConditionContributionComponent* contributions =
            ecs::try_get<ObjectModelConditionContributionComponent>(
                registry, candidate.entity);
        if (contributions) {
            for (size_t source = 0; source < contributions->selected.size();
                 ++source) {
                addMask(selected, contributions->selected[source]);
            }
        }

        // The force switches disable this producer; they do not grant it
        // ownership of an identically named bit set by a script, upgrade, or
        // specialized module. RefCode likewise leaves the corresponding flag
        // untouched when the global switch is disabled.
        game::ModelConditionMask ownedThisTick = catalog.owned;
        if (const ObjectSpawnSlaveComponent* spawnSlave =
                ecs::try_get<ObjectSpawnSlaveComponent>(
                    registry, candidate.entity);
            spawnSlave && !spawnSlave->tensileFormations.empty()) {
            addMask(ownedThisTick, catalog.postCollapse);
            addMask(ownedThisTick, catalog.freefall);
        }
        if (ecs::try_get<ObjectParticleUplinkComponent>(
                registry, candidate.entity)) {
            addMask(ownedThisTick, catalog.particleUplinkOwned);
        }
        if (contributions) {
            for (size_t source = 0; source < contributions->owned.size();
                 ++source) {
                addMask(ownedThisTick, contributions->owned[source]);
            }
        }
        if (!environment.forceModelsToFollowTimeOfDay &&
            !hasObjectNight)
        {
            ownedThisTick.clear(catalog.night);
        }
        if (!environment.forceModelsToFollowWeather &&
            !hasObjectSnow)
        {
            ownedThisTick.clear(catalog.snow);
        }
        game::ModelConditionMask next = visual.modelConditionFlags;
        next.clear(ownedThisTick);
        addMask(next, selected);
        if (next.words != visual.modelConditionFlags.words)
        {
            visual.modelConditionFlags = next;
            ++report.changedObjects;
            markObjectDirty(
                registry, candidate.entity,
                ObjectDirtyDomain::RenderExtraction);
        }
        clearObjectDirty(
            registry, candidate.entity,
            ObjectDirtyDomain::ModelCondition);
    }
    return report;
}

} // namespace engine
