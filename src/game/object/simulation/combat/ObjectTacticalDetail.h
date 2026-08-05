#pragma once

#include "game/object/simulation/combat/ObjectTactical.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "core/math/fixed/q32_32_trig.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/contracts/ObjectToppleMath.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>


namespace engine::object_tactical_detail {

struct QueuedToppleRequest final {
    ObjectToppleRequest request;
    uint64_t submissionOrdinal = 0;
};

struct ObjectToppleJournal final {
    container::Vector<QueuedToppleRequest> pending;
    uint64_t nextSubmissionOrdinal = 1;
};

struct ObjectPropagandaAuraProjection final {
    game::WeaponBonusConditionMask conditions = 0;
};

[[nodiscard]] inline uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

[[nodiscard]] inline uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] inline uint32_t variedMilliseconds(
    uint32_t milliseconds, math::q32_32 factor,
    SimulationRandom* random) noexcept {
    if (milliseconds == 0 || !random || factor <= math::q32_32{})
        return milliseconds;
    const math::q32_32 zero{};
    const math::q32_32 one{int32_t{1}};
    const math::q32_32 variation = math::q32_32::max(zero, factor);
    const math::q32_32 scale = random->fixedInclusive(
        math::q32_32::max(zero, one - variation), one + variation);
    const uint64_t raw = static_cast<uint64_t>(scale.raw());
    const uint64_t whole = raw >> 32u;
    const uint64_t fraction = raw & ((uint64_t{1} << 32u) - 1u);
    if (whole > std::numeric_limits<uint32_t>::max() /
                    static_cast<uint64_t>(milliseconds)) {
        return std::numeric_limits<uint32_t>::max();
    }
    const uint64_t scaledWhole =
        static_cast<uint64_t>(milliseconds) * whole;
    const uint64_t scaledFraction =
        (static_cast<uint64_t>(milliseconds) * fraction) >> 32u;
    return static_cast<uint32_t>(std::min<uint64_t>(
        scaledWhole + scaledFraction,
        std::numeric_limits<uint32_t>::max()));
}

inline constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] inline bool containsToken(container::StringView value,
                                 container::StringView token) noexcept {
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n", cursor);
        if (equalInsensitive(value.substr(cursor, end == container::StringView::npos
            ? value.size() - cursor : end - cursor), token)) return true;
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return false;
}

[[nodiscard]] inline bool hasKind(const ecs::registry& registry, ecs::entity entity,
                           game::ObjectKindOf sought) noexcept {
    const ObjectKindOfComponent* kinds = ecs::try_get<ObjectKindOfComponent>(registry, entity);
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] inline bool sameMapStatus(const ecs::registry& registry,
                                 ecs::entity left, ecs::entity right) noexcept {
    const auto* a = ecs::try_get<ObjectMapStatusComponent>(registry, left);
    const auto* b = ecs::try_get<ObjectMapStatusComponent>(registry, right);
    return (a && a->offMap) == (b && b->offMap);
}

[[nodiscard]] inline bool alive(const ecs::registry& registry,
                         const ObjectLifecycle& lifecycle,
                         ObjectId object, ecs::entity entity) noexcept {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const auto* life = ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const auto* health = ecs::try_get<ObjectHealthComponent>(registry, entity);
    const auto* map = ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return (!life || life->phase == ObjectLifecyclePhase::Alive) &&
        (!health || !health->effectivelyDead) && (!map || !map->offMap);
}

[[nodiscard]] inline bool hasAnyDamageWeapon(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) noexcept {
    const ObjectCombatProfileComponent* combat =
        ecs::try_get<ObjectCombatProfileComponent>(registry, entity);
    if (!combat || !combat->profile) return false;
    const game::WeaponSetProfile* active =
        combat->profile->findBestWeaponSet(combat->weaponConditions);
    if (!active) return false;
    for (const game::WeaponSlotProfile& slot : active->slots) {
        if (!slot.hasWeapon()) continue;
        const game::WeaponTemplate* weapon =
            content.findWeapon(slot.weaponTemplateName);
        if (weapon &&
            (weapon->fixed.primaryDamage > math::q32_32{} ||
             weapon->fixed.secondaryDamage > math::q32_32{})) {
            return true;
        }
    }
    return false;
}

inline void setModelCondition(ecs::registry& registry, ecs::entity entity,
                       game::ModelConditionFlag flag, bool enabled,
                       uint64_t confirmedTick = 0) {
    if (!ecs::try_get<RenderModelComponent>(registry, entity)) return;
    const game::ModelConditionMask mask = game::modelConditionMaskOf(flag);
    const game::ModelConditionMask empty;
    publishObjectModelConditionContribution(
        registry, entity, ObjectModelConditionContributionSource::Tactical,
        enabled ? empty : mask, enabled ? mask : empty, confirmedTick);
}

// RefCode SpecialAbilityUpdate::startPreparation selects the preparation
// animation per special-power kind, not for every ability: infantry capture
// raises a flag (SpecialAbilityUpdate.cpp:1026-1027) while the Hacker and the
// three Black Lotus hacks share the looping FIRING_A "typing" pose
// (SpecialAbilityUpdate.cpp:1063-1064). Every other ability leaves the
// preparation pose to its own ConditionStates.
[[nodiscard]] inline std::optional<game::ModelConditionFlag>
specialAbilityPreparationCondition(game::SpecialPowerType type) noexcept {
    switch (type) {
    case game::SpecialPowerType::InfantryCaptureBuilding:
        return game::ModelConditionFlag::RaisingFlag;
    case game::SpecialPowerType::HackerDisableBuilding:
    case game::SpecialPowerType::BlackLotusCaptureBuilding:
    case game::SpecialPowerType::BlackLotusDisableVehicleHack:
    case game::SpecialPowerType::BlackLotusStealCashHack:
        return game::ModelConditionFlag::FiringA;
    default:
        return std::nullopt;
    }
}

// The pair of poses above is mutually exclusive per ability, so a producer
// that leaves preparation must retire both. RefCode does exactly this in
// initiateIntentToDoSpecialPower (:499-500), onExit (:599-600) and
// startPacking (:741-743).
inline void clearSpecialAbilityPreparationConditions(
    ecs::registry& registry, ecs::entity entity, uint64_t confirmedTick) {
    setModelCondition(registry, entity, game::ModelConditionFlag::FiringA,
                      false, confirmedTick);
    setModelCondition(registry, entity, game::ModelConditionFlag::RaisingFlag,
                      false, confirmedTick);
}

inline void setBattlePlanDoorCondition(ecs::registry& registry, ecs::entity entity,
                                size_t doorSlot,
                                ObjectModelConditionDoorPhase phase,
                                uint64_t confirmedTick = 0) {
    publishObjectModelConditionDoor(
        registry, entity, ObjectModelConditionDoorSource::BattlePlan,
        doorSlot, phase, confirmedTick);
}

[[nodiscard]] inline game::ObjectStatusMask statusBit(game::ObjectStatusFlag flag) noexcept {
    return game::objectStatusBit(flag);
}

[[nodiscard]] inline bool memberMatchesBattlePlan(
    const ecs::registry& registry, ecs::entity entity,
    const game::ObjectBattlePlanRule& rule) noexcept {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    if (!kinds) return rule.validMemberKinds.none();
    return (rule.validMemberKinds.none() ||
            kinds->mask.test_for_any(rule.validMemberKinds)) &&
           kinds->mask.test_for_none(rule.invalidMemberKinds);
}

[[nodiscard]] inline const container::String* buttonField(
    const game::CommandButtonTemplate& button, container::StringView key) noexcept {
    for (auto found = button.fields.rbegin(); found != button.fields.rend(); ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    return nullptr;
}

[[nodiscard]] inline game::WeaponSlot buttonWeaponSlot(
    const game::CommandButtonTemplate& button) noexcept {
    const container::String* value = buttonField(button, "WeaponSlot");
    if (value && equalInsensitive(*value, "SECONDARY")) return game::WeaponSlot::Secondary;
    if (value && equalInsensitive(*value, "TERTIARY")) return game::WeaponSlot::Tertiary;
    return game::WeaponSlot::Primary;
}

inline void insertSystemOrder(ObjectOrderQueueComponent& queue, ObjectOrderIntent order,
                       ObjectOrderSystemPurpose purpose, uint32_t instance) {
    while (!queue.orders.empty() && queue.orders.front().source == ObjectOrderSource::System &&
           queue.orders.front().systemPurpose == purpose &&
           queue.orders.front().systemPurposeInstance == instance) {
        queue.orders.erase(queue.orders.begin());
    }
    order.systemPurpose = purpose;
    order.systemPurposeInstance = instance;
    queue.orders.insert(queue.orders.begin(), std::move(order));
    ++queue.revision;
}

[[nodiscard]] inline uint32_t ruleAnimationMilliseconds(
    const game::ObjectBattlePlanRule& rule,
    game::ObjectBattlePlanStatus plan) noexcept {
    switch (plan) {
    case game::ObjectBattlePlanStatus::Bombardment: return rule.bombardmentAnimationMilliseconds;
    case game::ObjectBattlePlanStatus::HoldTheLine: return rule.holdTheLineAnimationMilliseconds;
    case game::ObjectBattlePlanStatus::SearchAndDestroy: return rule.searchAndDestroyAnimationMilliseconds;
    case game::ObjectBattlePlanStatus::None: return rule.transitionIdleMilliseconds;
    }
    return 0;
}

[[nodiscard]] inline size_t battlePlanDoorSlot(
    game::ObjectBattlePlanStatus plan) noexcept {
    switch (plan) {
    case game::ObjectBattlePlanStatus::Bombardment: return 0;
    case game::ObjectBattlePlanStatus::HoldTheLine: return 1;
    case game::ObjectBattlePlanStatus::SearchAndDestroy: return 2;
    case game::ObjectBattlePlanStatus::None: return 0;
    }
    return 0;
}

struct BattlePlanPresentationFields final {
    container::StringView unpackSound;
    container::StringView packSound;
    container::StringView idleLoopSound;
    container::StringView announcement;
    container::StringView messageLabel;
};

[[nodiscard]] inline BattlePlanPresentationFields battlePlanPresentationFields(
    const game::ObjectBattlePlanRule& rule,
    game::ObjectBattlePlanStatus plan) noexcept {
    switch (plan) {
    case game::ObjectBattlePlanStatus::Bombardment:
        return {rule.bombardmentUnpackSound, rule.bombardmentPackSound, {},
                rule.bombardmentAnnouncement,
                rule.bombardmentMessageLabel};
    case game::ObjectBattlePlanStatus::HoldTheLine:
        return {rule.holdTheLineUnpackSound, rule.holdTheLinePackSound, {},
                rule.holdTheLineAnnouncement,
                rule.holdTheLineMessageLabel};
    case game::ObjectBattlePlanStatus::SearchAndDestroy:
        return {rule.searchAndDestroyUnpackSound,
                rule.searchAndDestroyPackSound,
                rule.searchAndDestroyIdleLoopSound,
                rule.searchAndDestroyAnnouncement,
                rule.searchAndDestroyMessageLabel};
    case game::ObjectBattlePlanStatus::None:
        return {};
    }
    return {};
}

inline void applyBattlePlanMaximumHealth(
    ecs::registry& registry, ecs::entity entity,
    const game::ObjectBattlePlanRule& rule, bool enable,
    const ObjectSimulationRules& rules) noexcept {
    using Scalar = ObjectHealthComponent::Scalar;
    ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    const Scalar scalar = rule.strategyCenterHoldHealthScalar;
    if (!health || scalar <= Scalar{} || scalar == Scalar{int32_t{1}}) return;

    const Scalar previousMaximum = health->maximumFixed;
    if (previousMaximum <= Scalar{}) return;
    const Scalar nextMaximum = enable
        ? previousMaximum * scalar : previousMaximum / scalar;
    if (nextMaximum <= Scalar{}) return;

    const Scalar previousCurrent = health->currentFixed;
    Scalar nextCurrent = previousCurrent;
    switch (rule.strategyCenterHoldHealthChangeType) {
    case game::ObjectMaxHealthChangeType::PreserveRatio:
        nextCurrent = nextMaximum * (previousCurrent / previousMaximum);
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
    nextCurrent = Scalar::min(
        nextMaximum, Scalar::max(Scalar{}, nextCurrent));
    health->previousFixed = previousCurrent;
    health->maximumFixed = nextMaximum;
    health->initialFixed = nextMaximum;
    health->currentFixed = nextCurrent;
    health->damageState =
        objectBodyDamageStateFor(nextCurrent, nextMaximum, rules);
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity)) {
        projectObjectBodyDamageVisual(
            registry, entity, health->damageState, *visual);
    }
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
}

inline void applyStrategyCenterBattlePlan(
    ecs::registry& registry, ecs::entity entity,
    const game::ObjectBattlePlanRule& rule,
    game::ObjectBattlePlanStatus plan, bool enable,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick) noexcept {
    if (plan == game::ObjectBattlePlanStatus::HoldTheLine) {
        applyBattlePlanMaximumHealth(registry, entity, rule, enable, rules);
        return;
    }
    if (plan != game::ObjectBattlePlanStatus::SearchAndDestroy) return;

    const math::q32_32 scalar = rule.strategyCenterSearchSightScalar;
    if (scalar > math::q32_32{} && scalar != math::q32_32{int32_t{1}}) {
        const math::q32_32 vision =
            effectiveObjectVisionRangeFixed(registry, entity);
        const math::q32_32 shroud =
            effectiveObjectShroudClearingRangeFixed(registry, entity);
        setObjectVisionRangeOverride(
            registry, entity,
            enable ? vision * scalar : vision / scalar,
            enable ? shroud * scalar : shroud / scalar);
    }
    if (rule.strategyCenterDetectsStealth) {
        if (ObjectStealthDetectorComponent* detector =
                ecs::try_get<ObjectStealthDetectorComponent>(registry,
                                                               entity)) {
            detector->enabled = enable;
            detector->nextScanTick = enable
                ? confirmedTick : std::numeric_limits<uint64_t>::max();
        }
    }
}

inline void appendBattlePlanPresentationEvent(
    container::Vector<ObjectBattlePlanPresentationEvent>& events,
    ObjectBattlePlanPresentationPhase phase, ObjectId source,
    PlayerId owner, const game::ObjectBattlePlanRule& rule,
    game::ObjectBattlePlanStatus plan, uint64_t confirmedTick) {
    const BattlePlanPresentationFields fields =
        battlePlanPresentationFields(rule, plan);
    ObjectBattlePlanPresentationEvent event{
        .phase = phase,
        .source = source,
        .owner = owner,
        .plan = plan,
        .idleLoopSound = container::String{fields.idleLoopSound},
        .authoredOrder = rule.authoredOrder,
        .confirmedTick = confirmedTick,
    };
    switch (phase) {
    case ObjectBattlePlanPresentationPhase::Unpacking:
        event.transitionSound = fields.unpackSound;
        event.announcement = fields.announcement;
        event.messageLabel = fields.messageLabel;
        break;
    case ObjectBattlePlanPresentationPhase::Packing:
        event.transitionSound = fields.packSound;
        break;
    case ObjectBattlePlanPresentationPhase::Active:
        break;
    }
    events.push_back(std::move(event));
}

inline void rebuildBattlePlanProjections(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot* content, const ObjectSimulationRules& rules,
    SimulationRandom* random, uint64_t confirmedTick) {
    struct ActivePlan final {
        ObjectId source = INVALID_OBJECT_ID;
        PlayerId owner = INVALID_PLAYER_ID;
        const game::ObjectBattlePlanRule* rule = nullptr;
        game::ObjectBattlePlanStatus plan =
            game::ObjectBattlePlanStatus::None;
    };
    struct Provider final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Provider> providers;
    const auto providerView = ecs::view<
        const ObjectIdentityComponent, const OwnerComponent,
        const ObjectTacticalComponent>(registry);
    providers.reserve(providerView.size_hint());
    for (const ecs::entity entity : providerView) {
        const ObjectId object = providerView.template get<
            const ObjectIdentityComponent>(entity).id;
        if (alive(registry, lifecycle, object, entity)) {
            providers.push_back({object, entity});
        }
    }
    std::sort(providers.begin(), providers.end(),
              [](const Provider& left, const Provider& right) {
                  return left.object < right.object;
              });

    container::Vector<ActivePlan> activePlans;
    for (const Provider& provider : providers) {
        const ObjectTacticalComponent& tactical =
            ecs::get<const ObjectTacticalComponent>(registry,
                                                     provider.entity);
        const OwnerComponent& owner =
            ecs::get<const OwnerComponent>(registry, provider.entity);
        if (!owner.player || !tactical.plan) continue;
        const size_t count = std::min(tactical.battlePlans.size(),
                                      tactical.plan->battlePlans.size());
        for (size_t index = 0; index < count; ++index) {
            const ObjectBattlePlanRuntime& runtime =
                tactical.battlePlans[index];
            if (runtime.transition != ObjectBattlePlanTransition::Active ||
                runtime.current == game::ObjectBattlePlanStatus::None) {
                continue;
            }
            activePlans.push_back({
                .source = provider.object,
                .owner = owner.player,
                .rule = &tactical.plan->battlePlans[index],
                .plan = runtime.current,
            });
        }
    }

    // Snapshot and sort by ObjectId first: the transitions below consume the
    // shared SimulationRandom (setObjectWeaponBonusCondition restarts weapon
    // timers), so raw entt storage order would tie the RNG stream position to
    // the registry's dense layout.  Collecting also keeps the emplace below
    // from growing a pool the view is iterating.
    struct BattlePlanSubject { ObjectId id; ecs::entity entity; };
    container::Vector<BattlePlanSubject> subjects;
    {
        const auto subjectView =
            ecs::view<const ObjectIdentityComponent, const OwnerComponent>(registry);
        subjects.reserve(subjectView.size_hint());
        for (const ecs::entity entity : subjectView) {
            subjects.push_back(
                {subjectView.template get<const ObjectIdentityComponent>(entity).id,
                 entity});
        }
    }
    std::sort(subjects.begin(), subjects.end(),
              [](const BattlePlanSubject& a, const BattlePlanSubject& b) {
                  return a.id < b.id;
              });

    for (const BattlePlanSubject& subject : subjects) {
        const ecs::entity entity = subject.entity;
        const OwnerComponent& owner =
            ecs::get<const OwnerComponent>(registry, entity);
        const ActivePlan* selected = nullptr;
        for (const ActivePlan& plan : activePlans) {
            if (plan.owner == owner.player && plan.rule &&
                memberMatchesBattlePlan(registry, entity, *plan.rule)) {
                selected = &plan;
                break;
            }
        }
        game::WeaponBonusConditionMask desiredConditions = 0;
        math::q32_32 armor{int32_t{1}};
        math::q32_32 sight{int32_t{1}};
        if (selected) {
            switch (selected->plan) {
            case game::ObjectBattlePlanStatus::Bombardment:
                desiredConditions = game::weaponBonusConditionBit(
                    game::WeaponBonusCondition::BattleplanBombardment);
                break;
            case game::ObjectBattlePlanStatus::HoldTheLine:
                desiredConditions = game::weaponBonusConditionBit(
                    game::WeaponBonusCondition::BattleplanHoldTheLine);
                armor = selected->rule->holdTheLineArmorDamageScalar;
                break;
            case game::ObjectBattlePlanStatus::SearchAndDestroy:
                desiredConditions = game::weaponBonusConditionBit(
                    game::WeaponBonusCondition::BattleplanSearchAndDestroy);
                sight = selected->rule->searchAndDestroySightRangeScalar;
                break;
            case game::ObjectBattlePlanStatus::None:
                break;
            }
        }
        ObjectBattlePlanEffectComponent* effect =
            ecs::try_get<ObjectBattlePlanEffectComponent>(registry, entity);
        const game::WeaponBonusConditionMask previous =
            effect ? effect->weaponConditions : 0;
        for (const game::WeaponBonusCondition condition : {
                 game::WeaponBonusCondition::BattleplanBombardment,
                 game::WeaponBonusCondition::BattleplanHoldTheLine,
                 game::WeaponBonusCondition::BattleplanSearchAndDestroy}) {
            const game::WeaponBonusConditionMask bit =
                game::weaponBonusConditionBit(condition);
            if ((previous & bit) != (desiredConditions & bit)) {
                static_cast<void>(setObjectWeaponBonusCondition(
                    registry, entity, condition,
                    (desiredConditions & bit) != 0, content, random,
                    rules.logicFramesPerSecond, confirmedTick));
            }
        }
        if (!effect &&
            (desiredConditions != 0 || armor != math::q32_32{1} ||
             sight != math::q32_32{1})) {
            effect = &ecs::emplace<ObjectBattlePlanEffectComponent>(
                registry, entity);
        }
        if (!effect) continue;
        if (effect->weaponConditions != desiredConditions ||
            effect->armorDamageScalar != armor ||
            effect->sightRangeScalar != sight) {
            ++effect->revision;
        }
        effect->weaponConditions = desiredConditions;
        effect->armorDamageScalar = armor;
        effect->sightRangeScalar = sight;
        if (!selected && desiredConditions == 0 &&
            armor == math::q32_32{1} && sight == math::q32_32{1}) {
            ecs::remove<ObjectBattlePlanEffectComponent>(registry, entity);
        }
    }
}

} // namespace engine::object_tactical_detail
