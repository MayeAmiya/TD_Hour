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

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
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
[[nodiscard]] bool isActivationBlocked(const ecs::registry& registry, ecs::entity entity) noexcept
{
    const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
                                    game::objectStatusBit(game::ObjectStatusFlag::Destroyed));
}

[[nodiscard]] int asciiCompareIgnoreCase(container::StringView left, container::StringView right) noexcept
{
    const size_t common = std::min(left.size(), right.size());
    for (size_t index = 0; index < common; ++index)
    {
        const char a = container::asciiLower(left[index]);
        const char b = container::asciiLower(right[index]);
        if (a < b)
            return -1;
        if (a > b)
            return 1;
    }
    if (left.size() < right.size())
        return -1;
    if (left.size() > right.size())
        return 1;
    return 0;
}

using container::asciiEqualIgnoreCase;

[[nodiscard]] bool inventoryContains(
    const ObjectUpgradeInventoryComponent* inventory,
    UpgradeContentId upgrade) noexcept
{
    return inventory && upgradeMaskTest(inventory->completed, upgrade);
}

[[nodiscard]] bool inventoryInsert(
    ObjectUpgradeInventoryComponent& inventory,
    UpgradeContentId upgrade) noexcept
{
    if (!upgradeIdInMaskRange(upgrade) ||
        upgradeMaskTest(inventory.completed, upgrade)) return false;
    upgradeMaskSet(inventory.completed, upgrade);
    ++inventory.revision;
    return true;
}

[[nodiscard]] bool inventoryErase(
    ObjectUpgradeInventoryComponent& inventory,
    UpgradeContentId upgrade) noexcept
{
    if (!upgradeMaskTest(inventory.completed, upgrade)) return false;
    upgradeMaskClear(inventory.completed, upgrade);
    ++inventory.revision;
    return true;
}

[[nodiscard]] const UpgradeMask& objectInventory(
    const ecs::registry& registry, ecs::entity entity) noexcept
{
    static const UpgradeMask empty;
    const ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity);
    return inventory ? inventory->completed : empty;
}


// UpgradeMux::resetUpgrade never undoes an implementation.  It only makes a
// mux eligible to execute again if a later local grant re-establishes one of
// its activation bits.  Keep every migrated mux consumer in this transaction;
// a removal must not accidentally leave AutoHeal or FXListDie with a stale
// `m_upgradeExecuted` equivalent.
void resetMuxesForRemovedUpgrade(
    ecs::registry& registry, ecs::entity entity,
    UpgradeContentId removedUpgrade) noexcept
{
    if (ObjectUpgradeComponent* component = ecs::try_get<ObjectUpgradeComponent>(registry, entity);
        component && component->plan)
    {
        const size_t count = std::min(component->plan->rules.size(), component->instances.size());
        for (size_t index = 0; index < count; ++index)
        {
            ObjectUpgradeRuntime& runtime = component->instances[index];
            if (runtime.activated && upgradeMaskTest(
                    component->plan->rules[index].triggeredByMask,
                    removedUpgrade))
            {
                runtime.activated = false;
            }
        }
    }

    if (ObjectAutoHealComponent* component = ecs::try_get<ObjectAutoHealComponent>(registry, entity);
        component && component->plan)
    {
        const size_t count = std::min(component->plan->rules.size(), component->instances.size());
        for (size_t index = 0; index < count; ++index)
        {
            ObjectAutoHealRuntime& runtime = component->instances[index];
            if (runtime.upgradeActivated && upgradeMaskTest(
                    component->plan->rules[index].triggeredByMask,
                    removedUpgrade))
            {
                runtime.upgradeActivated = false;
            }
        }
    }

    if (ObjectFireWeaponWhenDamagedComponent* component =
            ecs::try_get<ObjectFireWeaponWhenDamagedComponent>(registry, entity);
        component && component->plan) {
        const size_t count = std::min(component->plan->rules.size(),
                                      component->instances.size());
        for (size_t index = 0; index < count; ++index) {
            ObjectFireWeaponWhenDamagedRuntime& runtime =
                component->instances[index];
            if (runtime.upgradeActivated &&
                game::objectFireWeaponUpgradeTriggeredBy(
                    component->plan->rules[index].upgradeMux,
                    removedUpgrade)) {
                runtime.upgradeActivated = false;
            }
        }
    }

    if (ObjectFireOclAfterCooldownComponent* component =
            ecs::try_get<ObjectFireOclAfterCooldownComponent>(registry,
                                                               entity);
        component && component->plan) {
        const size_t count = std::min(component->plan->rules.size(),
                                      component->instances.size());
        for (size_t index = 0; index < count; ++index) {
            ObjectFireOclAfterCooldownRuntime& runtime =
                component->instances[index];
            if (runtime.upgradeActivated &&
                game::objectFireWeaponUpgradeTriggeredBy(
                    component->plan->rules[index].upgradeMux,
                    removedUpgrade)) {
                runtime.upgradeActivated = false;
            }
        }
    }

    const ObjectDeathReactionComponent* reactions = ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    ObjectFxListDieRuntimeComponent* runtime = ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity);
    if (reactions && reactions->plan) {
        if (runtime) {
            const size_t count = std::min(reactions->plan->rules.size(),
                                          runtime->rules.size());
            for (size_t index = 0; index < count; ++index) {
                const game::ObjectDeathReactionRule& rule =
                    reactions->plan->rules[index];
                if (rule.kind != game::ObjectDeathReactionKind::FxList ||
                    !rule.fxListDie) continue;
                if (runtime->rules[index].activated &&
                    upgradeMaskTest(rule.fxListDie->triggeredByMask,
                                    removedUpgrade)) {
                    runtime->rules[index].activated = false;
                }
            }
        }
        if (ObjectFireWeaponWhenDeadRuntimeComponent* fireDead =
                ecs::try_get<ObjectFireWeaponWhenDeadRuntimeComponent>(
                    registry, entity)) {
            const size_t count = std::min(reactions->plan->rules.size(),
                                          fireDead->rules.size());
            for (size_t index = 0; index < count; ++index) {
                const game::ObjectDeathReactionRule& rule =
                    reactions->plan->rules[index];
                if (rule.kind !=
                        game::ObjectDeathReactionKind::FireWeaponWhenDead ||
                    !rule.fireWeaponWhenDead) continue;
                if (fireDead->rules[index].upgradeActivated &&
                    game::objectFireWeaponUpgradeTriggeredBy(
                        rule.fireWeaponWhenDead->upgradeMux,
                        removedUpgrade)) {
                    fireDead->rules[index].upgradeActivated = false;
                }
            }
        }
    }
}

void processUpgradeRemovals(
    ecs::registry& registry, ecs::entity entity,
    const UpgradeMask& removals) noexcept
{
    ObjectUpgradeInventoryComponent* inventory = ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity);
    for (size_t bit = 0; bit < kUpgradeMaskBits; ++bit)
    {
        if (!removals.test(bit)) continue;
        const UpgradeContentId removed{
            static_cast<uint32_t>(bit + 1u)};
        // RefCode always performs the reset pass even when the object did not
        // own the bit (for example when `RemovesUpgrades` names PLAYER tech).
        // Never fold this under inventoryErase's result.
        if (inventory)
            static_cast<void>(inventoryErase(*inventory, removed));
        resetMuxesForRemovedUpgrade(registry, entity, removed);
    }
}

void refreshFxListDieConflicts(ecs::registry& registry,
                               ecs::entity entity,
                               const UpgradeMask& ownerCompletedUpgrades,
                               const UpgradeCatalog* catalog) noexcept
{
    const ObjectDeathReactionComponent* reactions = ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    ObjectFxListDieRuntimeComponent* runtime = ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity);
    if (!reactions || !reactions->plan || !runtime)
        return;
    const UpgradeMask& localCompleted = objectInventory(registry, entity);
    const size_t count = std::min(reactions->plan->rules.size(), runtime->rules.size());
    for (size_t index = 0; index < count; ++index)
    {
        const game::ObjectDeathReactionRule& rule = reactions->plan->rules[index];
        if (rule.kind != game::ObjectDeathReactionKind::FxList || !rule.fxListDie)
            continue;
        runtime->rules[index].playerConflict =
            game::objectFxListDieHasUpgradeConflict(
                *rule.fxListDie, ownerCompletedUpgrades, localCompleted, catalog);
    }
}

void refreshFireWeaponWhenDeadConflicts(
    ecs::registry& registry, ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades,
    const UpgradeCatalog* catalog) noexcept {
    const ObjectDeathReactionComponent* reactions =
        ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    ObjectFireWeaponWhenDeadRuntimeComponent* runtime =
        ecs::try_get<ObjectFireWeaponWhenDeadRuntimeComponent>(registry,
                                                               entity);
    if (!reactions || !reactions->plan || !runtime) return;
    const UpgradeMask& localCompleted =
        objectInventory(registry, entity);
    const size_t count = std::min(reactions->plan->rules.size(),
                                  runtime->rules.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectDeathReactionRule& rule =
            reactions->plan->rules[index];
        if (rule.kind !=
                game::ObjectDeathReactionKind::FireWeaponWhenDead ||
            !rule.fireWeaponWhenDead) continue;
        runtime->rules[index].playerConflict =
            game::objectFireWeaponUpgradeHasConflict(
                rule.fireWeaponWhenDead->upgradeMux,
                ownerCompletedUpgrades, localCompleted, catalog);
    }
}

enum class UpgradeMuxConsumer : uint8_t
{
    ObjectUpgrade,
    AutoHeal,
    FireWeaponWhenDamaged,
    FireOclAfterCooldown,
    FxListDie,
    FireWeaponWhenDead,
};

struct UpgradeMuxCandidate final
{
    UpgradeMuxConsumer consumer = UpgradeMuxConsumer::ObjectUpgrade;
    size_t index = 0;
    uint32_t authoredOrder = std::numeric_limits<uint32_t>::max();
};

[[nodiscard]] bool isEarlierCandidate(const UpgradeMuxCandidate& candidate, const UpgradeMuxCandidate& current) noexcept
{
    if (candidate.authoredOrder != current.authoredOrder)
    {
        return candidate.authoredOrder < current.authoredOrder;
    }
    // Authored order is unique for a final recipe.  Keep a deterministic
    // fallback for malformed/generated content rather than depending on ECS
    // storage order.
    return static_cast<uint8_t>(candidate.consumer) < static_cast<uint8_t>(current.consumer);
}

[[nodiscard]] UpgradeMuxCandidate nextUpgradeMuxCandidate(const ObjectUpgradeComponent* upgrades,
                                                          size_t& upgradeIndex,
                                                          const ObjectAutoHealComponent* autoHeal,
                                                          size_t& autoHealIndex,
                                                          const ObjectFireWeaponWhenDamagedComponent* fireDamaged,
                                                          size_t& fireDamagedIndex,
                                                          const ObjectFireOclAfterCooldownComponent* fireOclCooldown,
                                                          size_t& fireOclCooldownIndex,
                                                          const ObjectDeathReactionComponent* reactions,
                                                          size_t& reactionIndex,
                                                          size_t& fireDeadIndex) noexcept
{
    UpgradeMuxCandidate next;
    bool found = false;
    if (upgrades && upgrades->plan && upgradeIndex < upgrades->plan->rules.size())
    {
        next = {.consumer = UpgradeMuxConsumer::ObjectUpgrade,
                .index = upgradeIndex,
                .authoredOrder = upgrades->plan->rules[upgradeIndex].authoredOrder};
        found = true;
    }
    if (autoHeal && autoHeal->plan && autoHealIndex < autoHeal->plan->rules.size())
    {
        const UpgradeMuxCandidate candidate{
            .consumer = UpgradeMuxConsumer::AutoHeal,
            .index = autoHealIndex,
            .authoredOrder = autoHeal->plan->rules[autoHealIndex].authoredOrder,
        };
        if (!found || isEarlierCandidate(candidate, next))
            next = candidate;
        found = true;
    }
    if (fireDamaged && fireDamaged->plan &&
        fireDamagedIndex < fireDamaged->plan->rules.size()) {
        const UpgradeMuxCandidate candidate{
            .consumer = UpgradeMuxConsumer::FireWeaponWhenDamaged,
            .index = fireDamagedIndex,
            .authoredOrder =
                fireDamaged->plan->rules[fireDamagedIndex].authoredOrder,
        };
        if (!found || isEarlierCandidate(candidate, next)) next = candidate;
        found = true;
    }
    if (fireOclCooldown && fireOclCooldown->plan &&
        fireOclCooldownIndex < fireOclCooldown->plan->rules.size()) {
        const UpgradeMuxCandidate candidate{
            .consumer = UpgradeMuxConsumer::FireOclAfterCooldown,
            .index = fireOclCooldownIndex,
            .authoredOrder = fireOclCooldown->plan->rules[
                fireOclCooldownIndex].authoredOrder,
        };
        if (!found || isEarlierCandidate(candidate, next)) next = candidate;
        found = true;
    }
    while (reactions && reactions->plan && reactionIndex < reactions->plan->rules.size())
    {
        const game::ObjectDeathReactionRule& rule = reactions->plan->rules[reactionIndex];
        if (rule.kind == game::ObjectDeathReactionKind::FxList && rule.fxListDie)
            break;
        ++reactionIndex;
    }
    if (reactions && reactions->plan && reactionIndex < reactions->plan->rules.size())
    {
        const UpgradeMuxCandidate candidate{
            .consumer = UpgradeMuxConsumer::FxListDie,
            .index = reactionIndex,
            .authoredOrder = reactions->plan->rules[reactionIndex].authoredOrder,
        };
        if (!found || isEarlierCandidate(candidate, next))
            next = candidate;
        found = true;
    }
    while (reactions && reactions->plan &&
           fireDeadIndex < reactions->plan->rules.size()) {
        const game::ObjectDeathReactionRule& rule =
            reactions->plan->rules[fireDeadIndex];
        if (rule.kind ==
                game::ObjectDeathReactionKind::FireWeaponWhenDead &&
            rule.fireWeaponWhenDead) break;
        ++fireDeadIndex;
    }
    if (reactions && reactions->plan &&
        fireDeadIndex < reactions->plan->rules.size()) {
        const UpgradeMuxCandidate candidate{
            .consumer = UpgradeMuxConsumer::FireWeaponWhenDead,
            .index = fireDeadIndex,
            .authoredOrder =
                reactions->plan->rules[fireDeadIndex].authoredOrder,
        };
        if (!found || isEarlierCandidate(candidate, next)) next = candidate;
        found = true;
    }
    return found ? next : UpgradeMuxCandidate{};
}

void activateAllEligibleMuxes(ecs::registry& registry,
                              ecs::entity entity,
                              const UpgradeMask& ownerCompletedUpgrades,
                              const ObjectSimulationRules& rules,
                              uint64_t confirmedTick,
                              ObjectUpgradeExecutionContext context)
{
    if (isActivationBlocked(registry, entity))
        return;
    const UpgradeCatalog* catalog =
        context.content ? context.content->upgradeCatalog() : nullptr;

    ObjectUpgradeComponent* upgrades = ecs::try_get<ObjectUpgradeComponent>(registry, entity);
    ObjectAutoHealComponent* autoHeal = ecs::try_get<ObjectAutoHealComponent>(registry, entity);
    ObjectFireWeaponWhenDamagedComponent* fireDamaged =
        ecs::try_get<ObjectFireWeaponWhenDamagedComponent>(registry, entity);
    ObjectFireOclAfterCooldownComponent* fireOclCooldown =
        ecs::try_get<ObjectFireOclAfterCooldownComponent>(registry, entity);
    const ObjectDeathReactionComponent* reactions = ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    ObjectFxListDieRuntimeComponent* fxRuntime = ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity);
    ObjectFireWeaponWhenDeadRuntimeComponent* fireDead =
        ecs::try_get<ObjectFireWeaponWhenDeadRuntimeComponent>(registry,
                                                               entity);

    size_t upgradeIndex = 0;
    size_t autoHealIndex = 0;
    size_t fireDamagedIndex = 0;
    size_t fireOclCooldownIndex = 0;
    size_t reactionIndex = 0;
    size_t fireDeadIndex = 0;
    while (true)
    {
        const UpgradeMuxCandidate candidate =
            nextUpgradeMuxCandidate(upgrades, upgradeIndex, autoHeal,
                                    autoHealIndex, fireDamaged,
                                    fireDamagedIndex, fireOclCooldown,
                                    fireOclCooldownIndex, reactions,
                                    reactionIndex, fireDeadIndex);
        if (candidate.authoredOrder == std::numeric_limits<uint32_t>::max())
            break;

        switch (candidate.consumer)
        {
        case UpgradeMuxConsumer::ObjectUpgrade:
        {
            ++upgradeIndex;
            if (!upgrades || !upgrades->plan || candidate.index >= upgrades->instances.size())
                break;
            ObjectUpgradeRuntime& runtime = upgrades->instances[candidate.index];
            const game::ObjectUpgradeRule& rule = upgrades->plan->rules[candidate.index];
            if (runtime.activated ||
                !game::objectUpgradeMatches(rule, ownerCompletedUpgrades, objectInventory(registry, entity), catalog))
            {
                break;
            }
            const bool requiresEffectSink =
                (rule.operation ==
                     game::ObjectUpgradeOperation::ObjectCreation &&
                 !rule.objectCreationList.empty()) ||
                (rule.operation ==
                     game::ObjectUpgradeOperation::ReplaceObject &&
                 !rule.replacementObject.empty());
            // A default/diagnostic execution context may inspect or activate
            // ordinary value upgrades, but it must never permanently consume
            // a structural upgrade without its central gameplay work sink.
            if (requiresEffectSink &&
                (!context.content || !context.effects)) {
                break;
            }
            // UpgradeModule::performUpgradeFX precedes removal and the
            // concrete upgradeImplementation. Freeze the anchor now so a
            // ReplaceObjectUpgrade cannot erase the object before the client
            // receives its authored FXListUpgrade.
            if (!rule.upgradeFx.empty() && context.effects) {
                const ObjectIdentityComponent* identity =
                    ecs::try_get<ObjectIdentityComponent>(registry, entity);
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(registry, entity);
                if (identity && identity->id && transform) {
                    const ObjectPhysicsComponent* physics =
                        ecs::try_get<ObjectPhysicsComponent>(registry,
                                                             entity);
                    context.effects->queueObjectUpgradeFxInvocation({
                        .fxList = rule.upgradeFx,
                        .source = identity->id,
                        .position = readAuthoritativeObjectPosition(
                            registry, entity, *transform),
                        .rollRadians = physics && physics->ownsAttitude
                            ? physics->roll
                            : ObjectPhysicsComponent::Scalar{},
                        .pitchRadians = physics && physics->ownsAttitude
                            ? physics->pitch
                            : ObjectPhysicsComponent::Scalar{},
                        .yawRadians = physics && physics->ownsAttitude
                            ? physics->yaw
                            : readAuthoritativeObjectYaw(
                                  registry, entity, *transform),
                        .authoredOrder = rule.authoredOrder,
                        .emissionSequence = context.effects->
                            reserveGameplaySubmissionOrdinal(),
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            processUpgradeRemovals(
                registry, entity, rule.removesUpgradesMask);
            const UpgradeMask& localCompleted = objectInventory(registry, entity);
            applyRule(registry, entity, rule, ownerCompletedUpgrades, localCompleted,
                      rules, confirmedTick, context);
            runtime.activated = true;
            if (requiresEffectSink) return;
            break;
        }
        case UpgradeMuxConsumer::AutoHeal:
        {
            ++autoHealIndex;
            if (!autoHeal || !autoHeal->plan || candidate.index >= autoHeal->instances.size())
                break;
            ObjectAutoHealRuntime& runtime = autoHeal->instances[candidate.index];
            const game::ObjectAutoHealParameters& parameters = autoHeal->plan->rules[candidate.index];
            if (runtime.upgradeActivated || runtime.stopped ||
                !game::objectAutoHealUpgradeMatches(
                    parameters, ownerCompletedUpgrades, objectInventory(registry, entity), catalog))
            {
                break;
            }
            processUpgradeRemovals(
                registry, entity, parameters.removesUpgradesMask);
            // AutoHealBehavior::upgradeImplementation only wakes the update;
            // it does not undo an earlier pulse when a removal reset occurs.
            runtime.upgradeActivated = true;
            runtime.active = true;
            runtime.nextWakeTick = confirmedTick;
            break;
        }
        case UpgradeMuxConsumer::FireWeaponWhenDamaged:
        {
            ++fireDamagedIndex;
            if (!fireDamaged || !fireDamaged->plan ||
                candidate.index >= fireDamaged->instances.size()) break;
            ObjectFireWeaponWhenDamagedRuntime& runtime =
                fireDamaged->instances[candidate.index];
            const auto& parameters =
                fireDamaged->plan->rules[candidate.index];
            if (runtime.upgradeActivated ||
                !game::objectFireWeaponUpgradeMatches(
                    parameters.upgradeMux, ownerCompletedUpgrades,
                    objectInventory(registry, entity), catalog)) break;
            processUpgradeRemovals(registry, entity,
                                   parameters.upgradeMux.removesUpgradesMask);
            runtime.upgradeActivated = true;
            break;
        }
        case UpgradeMuxConsumer::FireOclAfterCooldown:
        {
            ++fireOclCooldownIndex;
            if (!fireOclCooldown || !fireOclCooldown->plan ||
                candidate.index >= fireOclCooldown->instances.size()) break;
            ObjectFireOclAfterCooldownRuntime& runtime =
                fireOclCooldown->instances[candidate.index];
            const auto& parameters =
                fireOclCooldown->plan->rules[candidate.index];
            if (runtime.upgradeActivated ||
                !game::objectFireWeaponUpgradeMatches(
                    parameters.upgradeMux, ownerCompletedUpgrades,
                    objectInventory(registry, entity), catalog)) break;
            processUpgradeRemovals(
                registry, entity,
                parameters.upgradeMux.removesUpgradesMask);
            // Source upgradeImplementation is intentionally empty; the raw
            // TriggeredBy/ConflictsWith predicate is evaluated by the Update
            // every frame. Retain only UpgradeMux execution/removal state.
            runtime.upgradeActivated = true;
            break;
        }
        case UpgradeMuxConsumer::FxListDie:
        {
            ++reactionIndex;
            if (!reactions || !reactions->plan || !fxRuntime || candidate.index >= fxRuntime->rules.size())
            {
                break;
            }
            const game::ObjectDeathReactionRule& rule = reactions->plan->rules[candidate.index];
            if (!rule.fxListDie)
                break;
            ObjectFxListDieRuleRuntime& runtime = fxRuntime->rules[candidate.index];
            if (runtime.activated ||
                !game::objectFxListDieUpgradeTriggersSatisfied(
                    *rule.fxListDie, ownerCompletedUpgrades, objectInventory(registry, entity), catalog) ||
                game::objectFxListDieHasUpgradeConflict(
                    *rule.fxListDie, ownerCompletedUpgrades, objectInventory(registry, entity), catalog))
            {
                break;
            }
            processUpgradeRemovals(
                registry, entity, rule.fxListDie->removesUpgradesMask);
            runtime.activated = true;
            break;
        }
        case UpgradeMuxConsumer::FireWeaponWhenDead:
        {
            ++fireDeadIndex;
            if (!reactions || !reactions->plan || !fireDead ||
                candidate.index >= fireDead->rules.size()) break;
            const game::ObjectDeathReactionRule& rule =
                reactions->plan->rules[candidate.index];
            if (!rule.fireWeaponWhenDead) break;
            ObjectFireWeaponWhenDeadRuleRuntime& runtime =
                fireDead->rules[candidate.index];
            if (runtime.upgradeActivated ||
                !game::objectFireWeaponUpgradeMatches(
                    rule.fireWeaponWhenDead->upgradeMux,
                    ownerCompletedUpgrades,
                    objectInventory(registry, entity), catalog)) break;
            processUpgradeRemovals(
                registry, entity,
                rule.fireWeaponWhenDead->upgradeMux.removesUpgradesMask);
            runtime.upgradeActivated = true;
            break;
        }
        }
    }
    refreshFxListDieConflicts(registry, entity, ownerCompletedUpgrades, catalog);
    refreshFireWeaponWhenDeadConflicts(registry, entity,
                                       ownerCompletedUpgrades, catalog);
}

} // namespace object_upgrade_detail
} // namespace engine
