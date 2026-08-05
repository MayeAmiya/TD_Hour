#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/economy/ObjectAutoDeposit.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

[[nodiscard]] int32_t firstCompletedBoost(
    const game::ObjectAutoDepositParameters& parameters,
    const PlayerRegistry& players, PlayerId player,
    const UpgradeCatalog* catalog) noexcept {
    for (const game::ObjectAutoDepositBoost& boost : parameters.upgradedBoosts) {
        if (catalog &&
            players.hasUpgradeComplete(player, boost.upgrade, *catalog)) {
            return boost.amount;
        }
    }
    return 0;
}

[[nodiscard]] bool isUnderConstruction(const ecs::registry& registry,
                                       ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
}

} // namespace

uint64_t ObjectAutoDepositSystem::millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t scaled = static_cast<uint64_t>(milliseconds) * framesPerSecond;
    return (scaled + 999u) / 1000u;
}

void ObjectAutoDepositSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->autoDepositPlan) return;

    ObjectAutoDepositComponent component{
        .plan = type->archetype->autoDepositPlan,
    };
    component.instances.reserve(component.plan->rules.size());
    for (const game::ObjectAutoDepositParameters& rule : component.plan->rules) {
        component.instances.push_back({
            .nextDepositTick = saturatingAdd(
                confirmedTick,
                millisecondsToTicks(rule.depositTimingMilliseconds,
                                    rules.logicFramesPerSecond)),
        });
    }
    if (ObjectAutoDepositComponent* existing =
            ecs::try_get<ObjectAutoDepositComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectAutoDepositComponent>(registry, entity,
                                                 std::move(component));
    }
}

void ObjectAutoDepositSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    PlayerRegistry* players, const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectAutoDepositEvent>& outEvents,
    const UpgradeCatalog* upgradeCatalog) const {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectAutoDepositComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            !lifecycle.entityFromId(identity.id)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });

    for (const Candidate& candidate : candidates) {
        ObjectAutoDepositComponent& component =
            ecs::get<ObjectAutoDepositComponent>(registry, candidate.entity);
        if (!component.plan ||
            isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            continue;
        }
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectAutoDepositParameters& rule =
                component.plan->rules[index];
            ObjectAutoDepositRuntime& runtime = component.instances[index];
            if (confirmedTick < runtime.nextDepositTick) continue;

            if (!runtime.initialized) {
                // Deliberately armed only after the first live update. This
                // is the source save-load guard, retained even though modern
                // save persistence is not implemented yet.
                runtime.captureBonusArmed = true;
                runtime.initialized = true;
            }
            runtime.nextDepositTick = saturatingAdd(
                confirmedTick,
                millisecondsToTicks(rule.depositTimingMilliseconds,
                                    rules.logicFramesPerSecond));

            if (!players || rule.depositAmount <= 0 ||
                isUnderConstruction(registry, candidate.entity)) {
                continue;
            }
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(registry, candidate.entity);
            const PlayerState* player = owner ? players->get(owner->player) : nullptr;
            if (!player || player->isNeutral()) continue;

            const int32_t boost =
                firstCompletedBoost(rule, *players, owner->player, upgradeCatalog);
            const int64_t amount = static_cast<int64_t>(rule.depositAmount) +
                                   static_cast<int64_t>(boost);
            if (amount <= 0) continue;
            const bool deposited = rule.actualMoney
                ? players->deposit(owner->player, amount)
                : false;
            if (deposited) {
                // RefCode deliberately credits ScoreKeeper with the authored
                // base DepositAmount, not the UpgradedBoost included in the
                // actual cash mutation.
                static_cast<void>(players->recordMoneyEarned(
                    owner->player,
                    static_cast<uint64_t>(rule.depositAmount),
                    confirmedTick));
            }
            outEvents.push_back({
                .kind = ObjectAutoDepositEventKind::PeriodicIncome,
                .object = candidate.id,
                .player = owner->player,
                .amount = amount,
                .baseAmount = rule.depositAmount,
                .upgradeBoost = boost,
                .authoredOrder = rule.authoredOrder,
                .actualMoney = rule.actualMoney,
                .deposited = deposited,
                .confirmedTick = confirmedTick,
            });
        }
    }
}

void ObjectAutoDepositSystem::onObjectOwnerChanged(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    PlayerRegistry& players, PlayerId newOwner,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectAutoDepositEvent>& outEvents) const {
    if (!object || lifecycle.isPendingDestroy(object)) return;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return;
    ObjectAutoDepositComponent* component =
        ecs::try_get<ObjectAutoDepositComponent>(registry, *entity);
    if (!component || !component->plan) return;

    const PlayerState* player = players.get(newOwner);
    if (!player || player->isNeutral()) return;
    const size_t count = std::min(component->plan->rules.size(),
                                  component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectAutoDepositParameters& rule =
            component->plan->rules[index];
        ObjectAutoDepositRuntime& runtime = component->instances[index];
        runtime.nextDepositTick = saturatingAdd(
            confirmedTick,
            millisecondsToTicks(rule.depositTimingMilliseconds,
                                rules.logicFramesPerSecond));
        if (!runtime.captureBonusArmed || rule.initialCaptureBonus <= 0) {
            continue;
        }

        const bool deposited = players.deposit(newOwner, rule.initialCaptureBonus);
        if (deposited) {
            static_cast<void>(players.recordMoneyEarned(
                newOwner,
                static_cast<uint64_t>(rule.initialCaptureBonus),
                confirmedTick));
        }
        outEvents.push_back({
            .kind = ObjectAutoDepositEventKind::InitialCaptureBonus,
            .object = object,
            .player = newOwner,
            .amount = rule.initialCaptureBonus,
            .baseAmount = rule.initialCaptureBonus,
            .authoredOrder = rule.authoredOrder,
            .actualMoney = true,
            .deposited = deposited,
            .confirmedTick = confirmedTick,
        });
        // Source clears the one-shot arm after the award path, even though
        // modern overflow protection may reject the actual cash mutation.
        runtime.captureBonusArmed = false;
    }
}

} // namespace engine
