#include "game/object/simulation/economy/ObjectEconomy.h"

#include "game/object/simulation/economy/ObjectEconomyDetail.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/base/GameBalanceConstants.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

using namespace engine::object_economy_detail;

namespace {

[[nodiscard]] uint64_t variedDurationTicks(
    uint32_t milliseconds, math::q32_32 variationFactor,
    uint32_t framesPerSecond, engine::ObjectId object,
    uint32_t authoredOrder, uint64_t phaseStartTick) noexcept {
    const uint64_t base = millisecondsToTicks(milliseconds, framesPerSecond);
    if (base == 0 || variationFactor <= math::q32_32{}) return base;

    // The legacy state used the mutable gameplay RNG.  A per-transition hash
    // keeps the same authored [1-factor, 1+factor] interval without making
    // EnTT iteration order or an unrelated random consumer part of the result.
    uint64_t hash = phaseStartTick ^
        (static_cast<uint64_t>(object.value) << 32u) ^ authoredOrder;
    hash ^= hash >> 30u;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27u;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31u;
    const int64_t signedUnitRaw =
        static_cast<int64_t>(static_cast<uint32_t>(hash)) * 2 -
        (int64_t{1} << 32u);
    const math::q32_32 factor = math::q32_32::clamp(
        variationFactor, math::q32_32{}, math::q32_32{int32_t{1}});
    const math::q32_32 scale = math::q32_32{int32_t{1}} +
        factor * math::q32_32::from_raw(signedUnitRaw);
    const uint64_t boundedBase = std::min<uint64_t>(
        base, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
    const int32_t varied =
        (math::q32_32{static_cast<int32_t>(boundedBase)} * scale).to_int();
    return static_cast<uint64_t>(std::max<int32_t>(0, varied));
}

void projectHackInternetPhase(ecs::registry& registry,
                              ecs::entity entity,
                              engine::ObjectHackInternetRuntime& runtime,
                              engine::ObjectHackInternetRuntimePhase phase,
                              uint64_t confirmedTick) {
    if (runtime.phase != phase) {
        runtime.phase = phase;
        ++runtime.revision;
    }
    if (!ecs::try_get<engine::RenderModelComponent>(registry, entity)) return;
    static const game::ModelConditionMask owned =
        game::modelConditionMaskOf(game::ModelConditionFlag::Unpacking, game::ModelConditionFlag::FiringA, game::ModelConditionFlag::Packing);
    static const game::ModelConditionMask unpacking =
        game::modelConditionMaskOf(game::ModelConditionFlag::Unpacking);
    static const game::ModelConditionMask hacking =
        game::modelConditionMaskOf(game::ModelConditionFlag::FiringA);
    static const game::ModelConditionMask packing =
        game::modelConditionMaskOf(game::ModelConditionFlag::Packing);
    game::ModelConditionMask selected;
    switch (phase) {
    case engine::ObjectHackInternetRuntimePhase::Unpacking:
        selected = unpacking;
        break;
    case engine::ObjectHackInternetRuntimePhase::Hacking:
        selected = hacking;
        break;
    case engine::ObjectHackInternetRuntimePhase::Packing:
        selected = packing;
        break;
    case engine::ObjectHackInternetRuntimePhase::Idle:
        break;
    }
    publishObjectModelConditionContribution(
        registry, entity,
        engine::ObjectModelConditionContributionSource::Economy,
        owned, selected, confirmedTick);
}

[[nodiscard]] uint32_t hackCashAmount(
    const game::ObjectHackInternetRule& rule,
    game::ObjectVeterancyLevel level) noexcept {
    if (level >= game::ObjectVeterancyLevel::Heroic && rule.heroicCashAmount)
        return rule.heroicCashAmount;
    if (level >= game::ObjectVeterancyLevel::Elite && rule.eliteCashAmount)
        return rule.eliteCashAmount;
    if (level >= game::ObjectVeterancyLevel::Veteran && rule.veteranCashAmount)
        return rule.veteranCashAmount;
    return rule.regularCashAmount ? rule.regularCashAmount : 1u;
}


} // namespace

namespace engine {

bool ObjectEconomySystem::requestHackInternet(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || hasBlockingStatus(registry, *entity, confirmedTick))
        return false;
    ObjectEconomyComponent* economy =
        ecs::try_get<ObjectEconomyComponent>(registry, *entity);
    if (!economy || !economy->plan || economy->hackInternet.empty()) {
        return false;
    }
    const ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, *entity);
    bool accepted = false;
    const size_t count = std::min(economy->plan->hackInternet.size(),
                                  economy->hackInternet.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectHackInternetRuntime& runtime = economy->hackInternet[index];
        if (runtime.phase == ObjectHackInternetRuntimePhase::Hacking ||
            runtime.phase == ObjectHackInternetRuntimePhase::Unpacking) {
            accepted = true;
            continue;
        }
        if (runtime.phase == ObjectHackInternetRuntimePhase::Packing) continue;
        const game::ObjectHackInternetRule& rule =
            economy->plan->hackInternet[index];
        runtime.autoStartedByContainment = false;
        runtime.observedExternalOrderRevision =
            queue ? queue->externalRevision : 0;
        runtime.phaseEndTick = saturatingAdd(
            confirmedTick,
            variedDurationTicks(rule.unpackTimeMilliseconds,
                                rule.packUnpackVariationFactor,
                                rules.logicFramesPerSecond, object,
                                rule.authoredOrder, confirmedTick));
        runtime.nextCashTick = 0;
        projectHackInternetPhase(registry, *entity, runtime,
                                 ObjectHackInternetRuntimePhase::Unpacking,
                                 confirmedTick);
        accepted = true;
    }
    return accepted;
}

void ObjectEconomySystem::updateHackInternet(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, const GameContentSnapshot& content,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    struct Hacker final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Hacker> hackers;
    const auto view =
        ecs::view<const ObjectIdentityComponent, ObjectEconomyComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectEconomyComponent& economy =
            view.template get<const ObjectEconomyComponent>(entity);
        if (!economy.plan || economy.hackInternet.empty() ||
            !isAliveObject(registry, lifecycle, entity, identity.id) ||
            hasBlockingStatus(registry, entity, confirmedTick)) {
            continue;
        }
        hackers.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(hackers.begin(), hackers.end(),
              [](const Hacker& left, const Hacker& right) {
                  return left.id < right.id;
              });

    ObjectExperienceSystem experience;
    for (const Hacker& hacker : hackers) {
        ObjectOrderQueueComponent* mutableQueue =
            ecs::try_get<ObjectOrderQueueComponent>(
                registry, hacker.entity);
        if (mutableQueue && !mutableQueue->orders.empty() &&
            isHackInternetCommandButton(
                mutableQueue->orders.front().kind,
                mutableQueue->orders.front().contentName,
                content.findCommandButton(
                    mutableQueue->orders.front().contentName)) &&
            requestHackInternet(
                registry, lifecycle, hacker.id, rules,
                confirmedTick)) {
            mutableQueue->orders.erase(mutableQueue->orders.begin());
            ++mutableQueue->revision;
        }
        ObjectEconomyComponent& economy =
            ecs::get<ObjectEconomyComponent>(registry, hacker.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, hacker.entity);
        const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, hacker.entity);
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, hacker.entity);

        ObjectId internetCenter = INVALID_OBJECT_ID;
        if (contained && contained->container) {
            const std::optional<ecs::entity> host =
                lifecycle.entityFromId(contained->container);
            if (host &&
                !isObjectDisabledBy(registry, *host,
                                    ObjectDisabledReason::Subdued,
                                    confirmedTick) &&
                hasEconomyModule(
                    ecs::try_get<ObjectEconomyComponent>(registry, *host),
                    game::ObjectEconomyModuleKind::InternetHackContain)) {
                internetCenter = contained->container;
            }
        }

        const size_t count = std::min(economy.plan->hackInternet.size(),
                                      economy.hackInternet.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectHackInternetRule& rule =
                economy.plan->hackInternet[index];
            ObjectHackInternetRuntime& runtime = economy.hackInternet[index];

            // InternetHackContain emits the same typed command exactly once
            // per containment edge. Retaining the host ObjectId prevents an
            // explicit player interruption from immediately auto-restarting.
            if (runtime.phase == ObjectHackInternetRuntimePhase::Idle &&
                internetCenter && runtime.internetCenter != internetCenter) {
                runtime.internetCenter = internetCenter;
                runtime.autoStartedByContainment = true;
                runtime.observedExternalOrderRevision =
                    queue ? queue->externalRevision : 0;
                runtime.phaseEndTick = saturatingAdd(
                    confirmedTick,
                    variedDurationTicks(
                        rule.unpackTimeMilliseconds,
                        rule.packUnpackVariationFactor,
                        rules.logicFramesPerSecond, hacker.id,
                        rule.authoredOrder, confirmedTick));
                runtime.nextCashTick = 0;
                projectHackInternetPhase(
                    registry, hacker.entity, runtime,
                    ObjectHackInternetRuntimePhase::Unpacking,
                    confirmedTick);
            } else if (!internetCenter &&
                       runtime.phase == ObjectHackInternetRuntimePhase::Idle) {
                runtime.internetCenter = INVALID_OBJECT_ID;
            }

            if (queue && runtime.phase != ObjectHackInternetRuntimePhase::Idle &&
                runtime.observedExternalOrderRevision !=
                    queue->externalRevision) {
                runtime.observedExternalOrderRevision = queue->externalRevision;
                runtime.autoStartedByContainment = false;
                runtime.nextCashTick = 0;
                if (internetCenter) {
                    // Legacy getPackTime() returns zero while contained.
                    runtime.phaseEndTick = confirmedTick;
                    projectHackInternetPhase(
                        registry, hacker.entity, runtime,
                        ObjectHackInternetRuntimePhase::Idle,
                        confirmedTick);
                } else {
                    runtime.phaseEndTick = saturatingAdd(
                        confirmedTick,
                        variedDurationTicks(
                            rule.packTimeMilliseconds,
                            rule.packUnpackVariationFactor,
                            rules.logicFramesPerSecond, hacker.id,
                            rule.authoredOrder, confirmedTick));
                    projectHackInternetPhase(
                        registry, hacker.entity, runtime,
                        ObjectHackInternetRuntimePhase::Packing,
                        confirmedTick);
                }
            }

            if (runtime.autoStartedByContainment && !internetCenter &&
                runtime.phase != ObjectHackInternetRuntimePhase::Idle) {
                runtime.autoStartedByContainment = false;
                runtime.internetCenter = INVALID_OBJECT_ID;
                runtime.phaseEndTick = confirmedTick;
                runtime.nextCashTick = 0;
                projectHackInternetPhase(registry, hacker.entity, runtime,
                                         ObjectHackInternetRuntimePhase::Idle,
                                         confirmedTick);
            }

            if (runtime.phase == ObjectHackInternetRuntimePhase::Unpacking &&
                runtime.phaseEndTick <= confirmedTick) {
                projectHackInternetPhase(registry, hacker.entity, runtime,
                                         ObjectHackInternetRuntimePhase::Hacking,
                                         confirmedTick);
                const uint32_t delay = internetCenter
                    ? rule.cashUpdateDelayFastMilliseconds
                    : rule.cashUpdateDelayMilliseconds;
                runtime.nextCashTick = saturatingAdd(
                    confirmedTick,
                    std::max<uint64_t>(
                        1u, millisecondsToTicks(delay,
                                               rules.logicFramesPerSecond)));
            } else if (runtime.phase ==
                           ObjectHackInternetRuntimePhase::Packing &&
                       runtime.phaseEndTick <= confirmedTick) {
                runtime.internetCenter = INVALID_OBJECT_ID;
                projectHackInternetPhase(registry, hacker.entity, runtime,
                                         ObjectHackInternetRuntimePhase::Idle,
                                         confirmedTick);
            }

            if (runtime.phase != ObjectHackInternetRuntimePhase::Hacking ||
                !owner || !owner->player || runtime.nextCashTick > confirmedTick) {
                continue;
            }
            if (isObjectDisabledBy(registry, hacker.entity,
                                   ObjectDisabledReason::Hacked,
                                   confirmedTick)) {
                runtime.nextCashTick = saturatingAdd(runtime.nextCashTick, 1u);
                continue;
            }

            const ObjectVeterancyComponent* veterancy =
                ecs::try_get<ObjectVeterancyComponent>(registry, hacker.entity);
            const game::ObjectVeterancyLevel level = veterancy
                ? veterancy->level
                : game::ObjectVeterancyLevel::Regular;
            const uint32_t amount = hackCashAmount(rule, level);
            if (players.adjustCash(owner->player, static_cast<int64_t>(amount))) {
                static_cast<void>(players.recordMoneyEarned(
                    owner->player, static_cast<uint64_t>(amount),
                    confirmedTick));
                ++runtime.revision;
                if (rule.xpPerCashUpdate != 0) {
                    const int32_t points = static_cast<int32_t>(
                        std::min<uint32_t>(rule.xpPerCashUpdate,
                                           static_cast<uint32_t>(
                                               std::numeric_limits<int32_t>::max())));
                    static_cast<void>(experience.addPoints(
                        registry, lifecycle, hacker.id, points, false,
                        confirmedTick));
                }
            }
            const uint32_t delay = internetCenter
                ? rule.cashUpdateDelayFastMilliseconds
                : rule.cashUpdateDelayMilliseconds;
            runtime.nextCashTick = saturatingAdd(
                confirmedTick,
                std::max<uint64_t>(
                    1u, millisecondsToTicks(delay,
                                           rules.logicFramesPerSecond)));
        }
    }
}

} // namespace engine
