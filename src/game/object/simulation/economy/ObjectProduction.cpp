#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/economy/ObjectProductionDetail.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/FactionTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/command/CommandBarOverrides.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>


namespace engine {
namespace production_detail {

using container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t scaled =
        static_cast<uint64_t>(milliseconds) * framesPerSecond;
    return (scaled + 999u) / 1000u;
}

[[nodiscard]] uint64_t nextTick(uint64_t tick) noexcept {
    return tick == std::numeric_limits<uint64_t>::max() ? tick : tick + 1u;
}

[[nodiscard]] uint64_t elapsedTicks(
    uint64_t now, uint64_t started) noexcept {
    return now >= started ? now - started : 0;
}

void setDoorPhase(ObjectProductionDoorRuntime& door,
                  ObjectProductionDoorPhase phase, uint64_t confirmedTick,
                  uint64_t visibleTick) noexcept {
    door.previousVisiblePhase = door.phase;
    door.phase = phase;
    door.phaseStartedTick = confirmedTick;
    door.conditionVisibleTick = visibleTick;
}

[[nodiscard]] bool advanceProductionPresentation(
    ObjectProductionComponent& component, uint64_t confirmedTick,
    uint32_t framesPerSecond) noexcept {
    if (!component.plan) return false;
    bool changed = false;
    const game::ObjectProductionPlan& plan = *component.plan;
    const uint64_t openingTicks = millisecondsToTicks(
        plan.doorOpeningMilliseconds, framesPerSecond);
    const uint64_t waitTicks = millisecondsToTicks(
        plan.doorWaitOpenMilliseconds, framesPerSecond);
    const uint64_t closingTicks = millisecondsToTicks(
        plan.doorClosingMilliseconds, framesPerSecond);

    const size_t doorCount = std::min<size_t>(
        plan.numberOfDoorAnimations, component.doors.size());
    for (size_t index = 0; index < doorCount; ++index) {
        ObjectProductionDoorRuntime& door = component.doors[index];
        const uint64_t age = elapsedTicks(
            confirmedTick, door.phaseStartedTick);
        switch (door.phase) {
        case ObjectProductionDoorPhase::Opening:
            if (age > openingTicks) {
                setDoorPhase(door, ObjectProductionDoorPhase::WaitingOpen,
                             confirmedTick, confirmedTick);
                changed = true;
            }
            break;
        case ObjectProductionDoorPhase::WaitingOpen:
            if (age > waitTicks && !door.holdOpen) {
                setDoorPhase(door, ObjectProductionDoorPhase::Closing,
                             confirmedTick, confirmedTick);
                changed = true;
            }
            break;
        case ObjectProductionDoorPhase::Closing:
            if (age > closingTicks && !door.holdOpen) {
                setDoorPhase(door, ObjectProductionDoorPhase::Closed,
                             confirmedTick, confirmedTick);
                changed = true;
            }
            break;
        case ObjectProductionDoorPhase::Closed:
            break;
        }
    }

    if (component.constructionCompleteActive &&
        elapsedTicks(confirmedTick,
                     component.constructionCompleteStartedTick) >
            millisecondsToTicks(plan.constructionCompleteMilliseconds,
                                framesPerSecond)) {
        component.constructionCompleteActive = false;
        changed = true;
    }
    return changed;
}

[[nodiscard]] bool prepareCompletedUnitExit(
    ObjectProductionComponent& component, ObjectProductionJob& job,
    uint64_t confirmedTick, bool& changed) noexcept {
    if (!component.constructionCompleteActive) {
        component.constructionCompleteActive = true;
        component.constructionCompleteStartedTick = confirmedTick;
        // ProductionUpdate sets this flag below its dirty-flags publication
        // block, so the Drawable first observes it next confirmed frame.
        component.constructionCompleteVisibleTick = nextTick(confirmedTick);
        changed = true;
    }

    const size_t doorCount = component.plan
        ? std::min<size_t>(component.plan->numberOfDoorAnimations,
                           component.doors.size())
        : 0u;
    if (doorCount == 0) return true;
    if (!job.exitDoorAssigned) {
        job.exitDoorIndex = static_cast<uint8_t>(
            (std::max<uint32_t>(1u, job.productionId) - 1u) % doorCount);
        job.exitDoorAssigned = true;
        changed = true;
    }
    ObjectProductionDoorRuntime& door =
        component.doors[job.exitDoorIndex];
    switch (door.phase) {
    case ObjectProductionDoorPhase::Closed:
        setDoorPhase(door, ObjectProductionDoorPhase::Opening,
                     confirmedTick, nextTick(confirmedTick));
        changed = true;
        return false;
    case ObjectProductionDoorPhase::Opening:
        return false;
    case ObjectProductionDoorPhase::Closing:
        // RefCode does not replay OPENING when production catches a closing
        // door; it pops directly to WAITING_OPEN and exits the unit.
        setDoorPhase(door, ObjectProductionDoorPhase::WaitingOpen,
                     confirmedTick, nextTick(confirmedTick));
        changed = true;
        return true;
    case ObjectProductionDoorPhase::WaitingOpen:
        // Each successful exit refreshes DoorWaitOpenTime.
        changed = changed || door.phaseStartedTick != confirmedTick;
        door.phaseStartedTick = confirmedTick;
        return true;
    }
    return true;
}

[[nodiscard]] const container::String* commandButtonFieldLast(
    const game::CommandButtonTemplate& button, container::StringView key) noexcept {
    for (auto found = button.fields.rbegin(); found != button.fields.rend(); ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    return nullptr;
}

[[nodiscard]] bool buttonScienceRequirementsSatisfied(const game::CommandButtonTemplate& button,
                                                        const PlayerRegistry& players,
                                                        PlayerId player) {
    // CommandButtonStore owns legacy `Science` parsing, including the None
    // sentinel.  Consume the compiled vector instead of reparsing raw fields
    // here, otherwise the authoritative producer and the ControlBar can
    // disagree on the same command.
    for (const container::String& science : button.sciences) {
        if (science.empty() || !players.hasScience(player, science)) return false;
    }
    return true;
}

[[nodiscard]] bool producerCanResearchUpgrade(
    ecs::registry& registry, ecs::entity producer, const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    const PlayerRegistry& players, PlayerId player, const UpgradeDefinition& upgrade,
    ObjectUpgradeProductionAdmission admission) {
    const container::StringView commandSetName =
        effectiveObjectCommandSetName(registry, producer);
    const game::CommandSetTemplate* commandSet = content.findCommandSet(commandSetName);
    if (!commandSet) return false;

    for (size_t slot = 0; slot < commandSet->commands.size(); ++slot) {
        const container::StringView buttonName = commandBarOverrides.effectiveButtonName(
            commandSet->name, slot, commandSet->commands[slot]);
        if (buttonName.empty()) continue;
        const game::CommandButtonTemplate* button = content.findCommandButton(buttonName);
        const game::CommandButtonKind expectedKind =
            upgrade.type == UpgradeDefinitionType::Player
            ? game::CommandButtonKind::PlayerUpgrade
            : game::CommandButtonKind::ObjectUpgrade;
        if (!button ||
            (admission == ObjectUpgradeProductionAdmission::PlayerCommand &&
             button->descriptor.kind != expectedKind)) {
            continue;
        }
        const container::String* buttonUpgrade = commandButtonFieldLast(*button, "Upgrade");
        // Upgrade identity follows the exact-case NameKey convention. The
        // command spelling itself remains legacy-insensitive, like the INI
        // parser's lookup-list fields.
        if (!buttonUpgrade || *buttonUpgrade != upgrade.name) continue;
        if (admission == ObjectUpgradeProductionAdmission::ScriptAi ||
            buttonScienceRequirementsSatisfied(*button, players, player)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool producerCanBuildUnit(
    ecs::registry& registry, ecs::entity producer, const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    const PlayerRegistry& players, PlayerId player,
    const game::ObjectArchetype& product, bool ignorePrerequisites) {
    const container::StringView commandSetName =
        effectiveObjectCommandSetName(registry, producer);
    const game::CommandSetTemplate* commandSet = content.findCommandSet(commandSetName);
    if (!commandSet) return false;

    for (size_t slot = 0; slot < commandSet->commands.size(); ++slot) {
        const container::StringView buttonName = commandBarOverrides.effectiveButtonName(
            commandSet->name, slot, commandSet->commands[slot]);
        if (buttonName.empty()) continue;
        const game::CommandButtonTemplate* button = content.findCommandButton(buttonName);
        if (!button ||
            (button->descriptor.kind != game::CommandButtonKind::UnitBuild &&
             button->descriptor.kind != game::CommandButtonKind::DozerConstruct)) {
            continue;
        }
        const container::String* objectName = commandButtonFieldLast(*button, "Object");
        const container::SharedPtr<const game::ObjectArchetype> buttonProduct =
            objectName ? content.findObjectArchetype(*objectName) : nullptr;
        if (!buttonProduct || !game::legacyThingTemplatesEquivalent(
                buttonProduct->templateData, product.templateData)) continue;
        if (ignorePrerequisites ||
            buttonScienceRequirementsSatisfied(*button, players, player)) return true;
    }
    return false;
}

[[nodiscard]] const UpgradeDefinition* frozenUpgrade(
    const GameContentSnapshot& content, const UpgradeDefinition& requested) noexcept {
    const UpgradeCatalog* catalog = content.upgradeCatalog();
    const UpgradeDefinition* frozen = catalog ? catalog->find(requested.id) : nullptr;
    if (!frozen || frozen->id != requested.id || frozen->name != requested.name ||
        frozen->type != requested.type) {
        return nullptr;
    }
    return frozen;
}

[[nodiscard]] bool producerIsSold(const ecs::registry& registry,
                                  ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Sold));
}

[[nodiscard]] ObjectProductionRequestResult rejected(
    ObjectProductionRejectionReason reason) noexcept {
    return {.accepted = false, .rejection = reason};
}

[[nodiscard]] const ProductionPercentModifier* findProductionModifier(
    const container::Vector<ProductionPercentModifier>& modifiers,
    container::StringView templateName) noexcept {
    const auto found = std::find_if(modifiers.begin(), modifiers.end(),
        [templateName](const ProductionPercentModifier& modifier) {
            return asciiEqualIgnoreCase(modifier.thingTemplateName, templateName);
        });
    return found == modifiers.end() ? nullptr : &*found;
}

[[nodiscard]] bool matchesCostModifierKinds(
    const game::ObjectArchetype& product,
    const game::ObjectKindOfMask& required) noexcept {
    // TEST_KINDOFMASK_MULTI(product, required, NONE) accepts an empty
    // required mask. Consequently an omitted/EffectKindOf=NONE rule is a
    // global modifier in RefCode rather than an inert malformed rule.
    return product.kindOfMask.test_for_all(required);
}

[[nodiscard]] math::q32_32 liveKindOfCostMultiplier(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerId player, const game::ObjectArchetype& product) {
    struct Modifier final {
        ObjectId object = INVALID_OBJECT_ID;
        uint32_t authoredOrder = 0;
        math::q32_32 percentage{};
        const game::ObjectKindOfMask* kinds = nullptr;
    };
    container::Vector<Modifier> modifiers;
    const auto view = ecs::view<
        const ObjectIdentityComponent, const OwnerComponent,
        const ObjectUpgradeComponent>(registry);
    for (const ecs::entity entity : view) {
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        if (owner.player != player) continue;
        const ObjectId object =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (!object || !lifecycle.entityFromId(object)) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (health && health->effectivelyDead) continue;
        const ObjectUpgradeComponent& upgrades =
            view.template get<const ObjectUpgradeComponent>(entity);
        if (!upgrades.plan) continue;
        const size_t count = std::min(upgrades.plan->rules.size(),
                                      upgrades.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectUpgradeRule& rule =
                upgrades.plan->rules[index];
            if (!upgrades.instances[index].activated ||
                rule.operation !=
                    game::ObjectUpgradeOperation::CostModifier ||
                !matchesCostModifierKinds(product,
                                          rule.costModifierKindMask)) {
                continue;
            }
            modifiers.push_back({
                .object = object,
                .authoredOrder = rule.authoredOrder,
                .percentage = rule.costModifierPercentage,
                .kinds = &rule.costModifierKindMask,
            });
        }
    }
    std::sort(modifiers.begin(), modifiers.end(),
        [](const Modifier& left, const Modifier& right) {
            if (left.object != right.object) return left.object < right.object;
            return left.authoredOrder < right.authoredOrder;
        });
    math::q32_32 multiplier{int32_t{1}};
    struct AppliedModifier final {
        math::q32_32 percentage{};
        const game::ObjectKindOfMask* kinds = nullptr;
    };
    container::Vector<AppliedModifier> applied;
    const auto sameKinds = [](const game::ObjectKindOfMask& left,
                              const game::ObjectKindOfMask& right) {
        return left == right;
    };
    for (const Modifier& modifier : modifiers) {
        if (!modifier.kinds)
            continue;
        const bool alreadyApplied = std::any_of(
            applied.begin(), applied.end(),
            [&](const AppliedModifier& existing) {
                return existing.kinds &&
                    existing.percentage == modifier.percentage &&
                    sameKinds(*existing.kinds, *modifier.kinds);
            });
        // Player::addKindOfProductionCostChange coalesces equal
        // (KindOfMask, Percentage) providers behind a refcount. The modifier
        // therefore applies once while at least one such provider is alive.
        if (alreadyApplied)
            continue;
        multiplier *= math::q32_32::max(
            math::q32_32{},
            math::q32_32{int32_t{1}} + modifier.percentage);
        applied.push_back({
            .percentage = modifier.percentage,
            .kinds = modifier.kinds,
        });
    }
    return multiplier;
}

[[nodiscard]] int64_t calculateUnitCost(const game::ObjectArchetype& product,
                                        const PlayerState& player,
                                        const ecs::registry& registry,
                                        const ObjectLifecycle& lifecycle) {
    int32_t multiplier = kBasisPointsPerWhole;
    if (const ProductionPercentModifier* modifier = findProductionModifier(
            player.productionModifiers.cost, product.name)) {
        multiplier = modifier->multiplierBasisPoints;
    }
    if (product.templateData.buildCostFixed <= math::q32_32{} ||
        multiplier <= 0) {
        return 0;
    }
    const math::q32_32 authoredCost = product.templateData.buildCostFixed;
    const math::q32_32 factionMultiplier = math::q32_32::from_fraction(
        multiplier, kBasisPointsPerWhole);
    const bool structure = game::objectHasKind(
        product.kindOfMask, game::ObjectKindOf::Structure);
    const math::q32_32 handicap = structure
        ? player.productionModifiers.structureBuildCostHandicap
        : player.productionModifiers.genericBuildCostHandicap;
    const math::q32_32 kindMultiplier = liveKindOfCostMultiplier(
        registry, lifecycle, player.id, product);
    const math::q32_32 cost = authoredCost * factionMultiplier *
        math::q32_32::max(math::q32_32{}, handicap) * kindMultiplier;
    if (cost <= math::q32_32{}) return 0;
    return static_cast<int64_t>(cost.to_int());
}

[[nodiscard]] uint32_t calculateUnitBuildFrames(const game::ObjectArchetype& product,
                                                 const PlayerState& player,
                                                 uint32_t framesPerSecond,
                                                 const EnergySimulationRules& energyRules,
                                                 uint64_t confirmedTick) noexcept {
    const int64_t secondsRaw = std::max<int64_t>(
        0, product.templateData.buildTimeSeconds.raw());
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t rawTimesRate = secondsRaw > 0 &&
        static_cast<uint64_t>(secondsRaw) >
            std::numeric_limits<uint64_t>::max() / rate
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(secondsRaw) * rate;
    uint64_t baseFrames = rawTimesRate >> 32u;
    const bool structure = game::objectHasKind(
        product.kindOfMask, game::ObjectKindOf::Structure);
    const math::q32_32 handicap = math::q32_32::max(
        math::q32_32{}, structure
            ? player.productionModifiers.structureBuildTimeHandicap
            : player.productionModifiers.genericBuildTimeHandicap);
    const uint64_t handicapRaw = static_cast<uint64_t>(handicap.raw());
    const uint64_t handicapInteger = handicapRaw >> 32u;
    const uint64_t handicapFraction = handicapRaw & 0xffffffffull;
    baseFrames = baseFrames * handicapInteger +
        ((baseFrames * handicapFraction) >> 32u);
    int32_t multiplier = kBasisPointsPerWhole;
    if (const ProductionPercentModifier* modifier = findProductionModifier(
            player.productionModifiers.time, product.name)) {
        multiplier = modifier->multiplierBasisPoints;
    }
    if (baseFrames == 0 || multiplier <= 0) return 1;
    const uint64_t positiveMultiplier = static_cast<uint32_t>(multiplier);
    const uint64_t scaled = baseFrames > std::numeric_limits<uint64_t>::max() / positiveMultiplier
        ? std::numeric_limits<uint64_t>::max()
        : baseFrames * positiveMultiplier;
    const uint64_t frames = scaled / static_cast<uint64_t>(kBasisPointsPerWhole);
    const uint32_t factionModifiedFrames = static_cast<uint32_t>(std::clamp<uint64_t>(
        frames, 1u, std::numeric_limits<uint32_t>::max()));
    // RefCode recalculates ThingTemplate::calcTimeToBuild every production
    // update. Keep the already migrated base/faction terms, then apply the
    // fixed session energy penalty at the same point in that calculation;
    // power never freezes at admission and a brownout slows rather than stops
    // a unit queue.
    return energyRules.adjustBuildFrames(
        factionModifiedFrames, player.energy.production, player.energy.consumption,
        player.energy.isSabotaged(confirmedTick));
}

[[nodiscard]] bool isAliveOwnedObject(
    const ecs::registry& registry, ecs::entity entity,
    PlayerId player) noexcept {
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return owner && owner->player == player && lifecycle &&
        lifecycle->phase == ObjectLifecyclePhase::Alive &&
        (!health || !health->effectivelyDead);
}

[[nodiscard]] bool isCompletedAliveOwnedObject(
    const ecs::registry& registry, ecs::entity entity,
    PlayerId player) noexcept {
    if (!isAliveOwnedObject(registry, entity, player)) return false;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return !status || !status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
}

[[nodiscard]] uint32_t existingBuildFacilityCount(
    const ecs::registry& registry, PlayerId player,
    const GameContentSnapshot& content,
    const game::ObjectArchetype& product) {
    if (product.templateData.buildCompletion !=
            game::ObjectBuildCompletion::AppearsAtRallyPoint ||
        product.templateData.prerequisiteObjectAlternatives.empty()) {
        return 0;
    }

    // ProductionPrerequisite::getExistingBuildFacilityTemplate considers
    // only the first Object alternative group and chooses its first owned
    // member. Count that exact facility template for MultipleFactory.
    const auto& facilityAlternatives =
        product.templateData.prerequisiteObjectAlternatives.front();
    const auto objects = ecs::view<const ThingTemplateComponent>(registry);
    for (const container::String& facilityName : facilityAlternatives) {
        const container::SharedPtr<const game::ObjectArchetype> facility =
            content.findObjectArchetype(facilityName);
        if (!facility) continue;
        uint64_t count = 0;
        for (const ecs::entity entity : objects) {
            if (!isCompletedAliveOwnedObject(registry, entity, player))
                continue;
            const ThingTemplateComponent& type =
                objects.template get<const ThingTemplateComponent>(entity);
            if (type.archetype && game::legacyThingTemplatesEquivalent(
                    type.archetype->templateData,
                    facility->templateData)) {
                ++count;
            }
        }
        if (count != 0) {
            return static_cast<uint32_t>(std::min<uint64_t>(
                count, std::numeric_limits<uint32_t>::max()));
        }
    }
    return 0;
}

[[nodiscard]] uint32_t calculateLiveUnitBuildFrames(
    const game::ObjectArchetype& product, const PlayerState& player,
    const ecs::registry& registry, const GameContentSnapshot& content,
    uint32_t framesPerSecond,
    const EnergySimulationRules& energyRules,
    uint64_t confirmedTick) {
    const uint32_t energyAdjusted = calculateUnitBuildFrames(
        product, player, framesPerSecond, energyRules, confirmedTick);
    return energyRules.adjustForMultipleFactories(
        energyAdjusted,
        existingBuildFacilityCount(
            registry, player.id, content, product));
}

[[nodiscard]] bool existingObjectCountsTowardLimit(
    const game::ObjectArchetype& candidate,
    const game::ObjectArchetype& product) noexcept {
    const container::String& linkKey =
        product.templateData.maxSimultaneousLinkKey;
    return linkKey.empty()
        ? game::legacyThingTemplatesEquivalent(
              candidate.templateData, product.templateData)
        : candidate.templateData.maxSimultaneousLinkKey == linkKey;
}

[[nodiscard]] uint32_t calculateUpgradeBuildFrames(const UpgradeDefinition& upgrade,
                                                    uint32_t framesPerSecond) noexcept {
    // UpgradeTemplate::calcTimeToBuild historically multiplies BuildTime by
    // LOGICFRAMES_PER_SECOND and truncates to an integer.  Keep the same
    // fixed-point boundary without introducing float into confirmed state;
    // unlike units, RefCode has no power/faction production-time modifier for
    // upgrades.  A malformed/zero time is modernized to one frame so a queue
    // can never divide by zero or become permanently stuck.
    const int64_t rawSeconds = upgrade.buildTimeSeconds.raw();
    if (rawSeconds <= 0) return 1;
    const uint64_t seconds = static_cast<uint64_t>(rawSeconds);
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t multiplied = seconds > std::numeric_limits<uint64_t>::max() / rate
        ? std::numeric_limits<uint64_t>::max()
        : seconds * rate;
    const uint64_t frames = multiplied >> 32u;
    return static_cast<uint32_t>(std::clamp<uint64_t>(
        frames, 1u, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] uint32_t quantityFor(const game::ObjectProductionPlan& plan,
                                   const game::ObjectArchetype& product) noexcept {
    // ProductionUpdate checks the authored modifiers in declaration order and
    // stops at the first equivalent ThingTemplate.  The modern compiler owns
    // one canonical template name per frozen archetype, so an exact
    // case-insensitive match is the stable value-only equivalent here.
    for (const game::ObjectProductionQuantityModifier& modifier : plan.quantityModifiers) {
        if (asciiEqualIgnoreCase(modifier.templateName, product.name)) {
            return std::max<uint32_t>(1, modifier.quantity);
        }
    }
    return 1;
}

[[nodiscard]] bool airfieldProductNeedsParking(
    const game::ObjectProductionExitPlan& exit,
    const game::ObjectArchetype& product) noexcept {
    if (exit.kind != game::ObjectProductionExitKind::AirfieldParking &&
        exit.kind != game::ObjectProductionExitKind::FlightDeck) {
        return false;
    }
    // ParkingPlaceBehavior returns DOOR_NONE_NEEDED for helipad products;
    // they launch from HeliPark01 and never consume a persistent slot.
    return !game::objectHasKind(product.kindOfMask,
                          game::ObjectKindOf::ProducedAtHelipad);
}

[[nodiscard]] bool hasAirfieldQueueCapacity(
    const ecs::registry& registry, ecs::entity producer,
    const ObjectProductionComponent& production,
    const game::ObjectArchetype& requested) noexcept {
    if (!production.exitPlan ||
        !airfieldProductNeedsParking(*production.exitPlan, requested)) {
        return true;
    }
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(registry, producer);
    if (!airfield) return false;
    size_t capacity = 0;
    size_t occupied = 0;
    const auto countSlots = [&capacity, &occupied](const auto& modules) {
        for (const auto& module : modules) {
            capacity += module.spaces.size();
            occupied += static_cast<size_t>(std::count_if(
                module.spaces.begin(), module.spaces.end(),
                [](ObjectId object) { return static_cast<bool>(object); }));
        }
    };
    countSlots(airfield->parkingPlaces);
    countSlots(airfield->flightDecks);
    uint64_t queuedReservations = 0;
    for (const ObjectProductionJob& job : production.jobs) {
        if (job.kind == ObjectProductionJobKind::Unit && job.product &&
            airfieldProductNeedsParking(*production.exitPlan,
                                        *job.product)) {
            queuedReservations += job.quantityTotal > job.quantityProduced
                ? job.quantityTotal - job.quantityProduced : 0u;
        }
    }
    const uint64_t requestedReservations = quantityFor(
        *production.plan, requested);
    const uint64_t available = capacity > occupied
        ? capacity - occupied : 0u;
    return queuedReservations <= available &&
        requestedReservations <= available - queuedReservations;
}

[[nodiscard]] uint32_t allocateProductionId(ObjectProductionComponent& component) noexcept {
    // Queue capacity is bounded by the authored ProductionUpdate plan, so a
    // local collision search is small even after the 32-bit legacy counter
    // wraps.  Never issue zero: it remains the external invalid sentinel.
    for (size_t attempt = 0; attempt <= component.jobs.size(); ++attempt) {
        uint32_t candidate = component.nextProductionId;
        ++component.nextProductionId;
        if (component.nextProductionId == 0) component.nextProductionId = 1;
        if (candidate == 0) continue;
        const bool occupied = std::any_of(component.jobs.begin(), component.jobs.end(),
            [candidate](const ObjectProductionJob& job) {
                return job.productionId == candidate;
            });
        if (!occupied) return candidate;
    }
    return 0;
}

void refundJob(PlayerRegistry& players, const ObjectProductionJob& job) noexcept {
    if (!job.payer || job.paidCost <= 0) return;
    // `paidCost` is the frozen price of the WHOLE quantity batch.  Legacy ZH
    // spawned an entire batch inside a single frame, so a cancel could never
    // observe a partially delivered job; ProductionUpdate instead spreads batch
    // spawns across ticks whenever an exit reservation (door delay, Queue
    // exitDelay, airfield parking) blocks a later unit.  Refunding the full
    // price in that window returned the money for units that had already left
    // the factory — a free-goods leak.  Refund only the undelivered share.
    int64_t refund = job.paidCost;
    if (job.quantityProduced > 0 && job.quantityTotal > 1) {
        if (job.quantityProduced >= job.quantityTotal) return;
        const int64_t remaining =
            static_cast<int64_t>(job.quantityTotal - job.quantityProduced);
        // Integer arithmetic keeps this deterministic across peers.  Cash and
        // batch sizes are both far below the range where the product could
        // overflow int64_t.
        refund = job.paidCost * remaining /
                 static_cast<int64_t>(job.quantityTotal);
        if (refund <= 0) return;
    }
    // PlayerRegistry::adjustCash performs a bounded positive addition.  That
    // is a deliberate modernization of Money::deposit: a full balance cannot
    // overflow and turn a valid cancelled job into corrupt economy state.
    static_cast<void>(players.adjustCash(job.payer, refund));
}

void releasePlayerUpgradeReservation(PlayerRegistry& players,
                                     const ObjectProductionJob& job) noexcept {
    if (job.kind != ObjectProductionJobKind::PlayerUpgrade || !job.payer ||
        job.upgradeName.empty()) {
        return;
    }
    static_cast<void>(players.cancelQueuedPlayerUpgrade(job.payer, job.upgrade));
}

void refundAndClear(PlayerRegistry& players, ObjectProductionComponent& component) noexcept {
    for (const ObjectProductionJob& job : component.jobs) {
        releasePlayerUpgradeReservation(players, job);
        refundJob(players, job);
    }
    if (!component.jobs.empty()) {
        component.jobs.clear();
        ++component.revision;
    }
}

[[nodiscard]] math::q32_32 normalizeProductionYaw(
    math::q32_32 yaw) noexcept {
    constexpr math::q32_32 pi =
        math::q32_32::from_raw(13'493'037'705ll);
    constexpr math::q32_32 twoPi =
        math::q32_32::from_raw(26'986'075'409ll);
    int64_t raw = yaw.raw() % twoPi.raw();
    if (raw > pi.raw()) raw -= twoPi.raw();
    if (raw <= -pi.raw()) raw += twoPi.raw();
    return math::q32_32::from_raw(raw);
}

[[nodiscard]] ObjectProductionRoutePoint transformLocalPoint(
    const LogicFixedVec3& producerPosition, math::q32_32 producerYaw,
    math::q32_32 localX, math::q32_32 localY,
    math::q32_32 localZ) noexcept {
    const math::q32_32_sincos rotation = math::fixed_sincos(producerYaw);
    return {
        .x = producerPosition.x +
              rotation.cosine * localX - rotation.sine * localY,
        .y = producerPosition.y +
              rotation.sine * localX + rotation.cosine * localY,
        .z = producerPosition.z + localZ,
    };
}

[[nodiscard]] ObjectProductionRoutePoint naturalRallyPoint(
    const LogicFixedVec3& producerPosition, math::q32_32 producerYaw,
    const game::ObjectProductionExitPlan& exitPlan) noexcept {
    math::q32_32 x = exitPlan.naturalRallyPointX;
    math::q32_32 y = exitPlan.naturalRallyPointY;
    math::q32_32 z = exitPlan.naturalRallyPointZ;
    const math::q32_32 length = math::q32_32::sqrt(
        x * x + y * y + z * z);
    if (length > math::q32_32{}) {
        const math::q32_32 scale =
            math::q32_32{int32_t{20}} / length;
        x += x * scale;
        y += y * scale;
        z += z * scale;
    }
    return transformLocalPoint(
        producerPosition, producerYaw, x, y, z);
}

[[nodiscard]] math::q32_32 fixedQuaternionYaw(
    const data::w3d::FixedQuaternion& rotation) noexcept {
    const math::q32_32 two{int32_t{2}};
    const math::q32_32 one{int32_t{1}};
    return math::fixed_atan2(
        two * (rotation.w * rotation.z + rotation.x * rotation.y),
        one - two * (rotation.y * rotation.y +
                     rotation.z * rotation.z));
}

[[nodiscard]] uint32_t takeExitReservationToken(
    ObjectProductionExitComponent& runtime) noexcept {
    uint32_t token = runtime.nextReservationToken++;
    if (token == 0) token = runtime.nextReservationToken++;
    if (runtime.nextReservationToken == 0) runtime.nextReservationToken = 1;
    return token;
}

[[nodiscard]] bool initializeSpawnPoints(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content,
    ObjectProductionExitComponent& runtime) {
    if (runtime.spawnPointsInitialized) return runtime.spawnPointCount != 0;
    if (!runtime.plan) return false;
    const container::String bonePrefix =
        runtime.plan->kind == game::ObjectProductionExitKind::AirfieldParking
            ? container::String{"HeliPark"}
            : runtime.plan->spawnPointBoneName;
    if (bonePrefix.empty()) return false;
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    const game::W3dPristineBoneCatalog* catalog =
        content.pristineBoneCatalog();
    if (!source || !source->archetype || !visual || !catalog ||
        !catalog->isLoaded()) {
        return false;
    }
    const game::ThingTemplate& templateData =
        source->archetype->templateData;
    const size_t visualRuleIndex = game::selectModelConditionVisualRuleIndex(
        templateData, visual->modelConditionFlags);
    if (visualRuleIndex >= templateData.modelConditionVisuals.size()) {
        return false;
    }
    uint8_t count = 0;
    for (uint32_t ordinal = 1;
         ordinal <= kObjectProductionSpawnPointCount; ++ordinal) {
        container::String name = bonePrefix;
        if (ordinal < 10) name.push_back('0');
        name += std::to_string(ordinal);
        const std::optional<data::w3d::FixedRigidTransform> bone =
            catalog->find(source->archetype->name, visualRuleIndex, name);
        if (!bone) break;
        ObjectProductionSpawnPointRuntime& point = runtime.spawnPoints[count++];
        point.localX = bone->translation.x;
        point.localY = bone->translation.y;
        point.localZ = bone->translation.z;
        point.localYaw = fixedQuaternionYaw(bone->rotation);
    }
    runtime.spawnPointCount = count;
    runtime.spawnPointsInitialized = true;
    ++runtime.revision;
    return count != 0;
}

[[nodiscard]] std::optional<ObjectProductionExitReservation> reserveExitRuntime(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ecs::entity entity,
    uint64_t confirmedTick, ObjectProductionExitComponent& runtime) {
    if (!runtime.plan) return std::nullopt;
    const game::ObjectProductionExitKind kind = runtime.plan->kind;
    if (kind == game::ObjectProductionExitKind::AirfieldParking) {
        // HeliPark01 is optional for a pure fixed-wing airfield. Resolve it
        // once when present; fixed-wing slots remain owned by ObjectAirfield.
        static_cast<void>(initializeSpawnPoints(
            registry, entity, content, runtime));
    }
    if (kind == game::ObjectProductionExitKind::SpawnPoint) {
        if (!initializeSpawnPoints(registry, entity, content, runtime)) {
            return std::nullopt;
        }
        for (uint8_t slot = 0; slot < runtime.spawnPointCount; ++slot) {
            ObjectProductionSpawnPointRuntime& point = runtime.spawnPoints[slot];
            if (point.occupier && !lifecycle.entityFromId(point.occupier)) {
                point.occupier = INVALID_OBJECT_ID;
                ++runtime.revision;
            }
            if (point.occupier || point.reservationToken != 0) continue;
            point.reservationToken = takeExitReservationToken(runtime);
            ++runtime.revision;
            return ObjectProductionExitReservation{
                .kind = kind,
                .token = point.reservationToken,
                .slot = slot,
                .localX = point.localX,
                .localY = point.localY,
                .localZ = point.localZ,
                .localYaw = point.localYaw,
                .hasLocalTransform = true,
            };
        }
        return std::nullopt;
    }
    if (kind != game::ObjectProductionExitKind::Queue) {
        return ObjectProductionExitReservation{
            .kind = kind,
            .token = takeExitReservationToken(runtime),
        };
    }
    if (runtime.activeReservationToken != 0) return std::nullopt;
    if (kind == game::ObjectProductionExitKind::Queue &&
        runtime.initialBurstRemaining == 0 &&
        confirmedTick < runtime.nextExitTick) {
        return std::nullopt;
    }
    runtime.activeReservationToken = takeExitReservationToken(runtime);
    ++runtime.revision;
    return ObjectProductionExitReservation{
        .kind = kind,
        .token = runtime.activeReservationToken,
    };
}

void releaseExitReservation(ObjectProductionExitComponent& runtime,
                            ObjectProductionExitReservation reservation) noexcept {
    if (!reservation || !runtime.plan ||
        reservation.kind != runtime.plan->kind) return;
    if (reservation.kind == game::ObjectProductionExitKind::SpawnPoint) {
        if (reservation.slot >= runtime.spawnPointCount) return;
        ObjectProductionSpawnPointRuntime& point =
            runtime.spawnPoints[reservation.slot];
        if (point.reservationToken != reservation.token) return;
        point.reservationToken = 0;
        ++runtime.revision;
        return;
    }
    if (reservation.kind != game::ObjectProductionExitKind::Queue) return;
    if (runtime.activeReservationToken != reservation.token) return;
    runtime.activeReservationToken = 0;
    ++runtime.revision;
}

[[nodiscard]] bool commitExitReservation(
    ObjectProductionExitComponent& runtime,
    ObjectProductionExitReservation reservation, ObjectId spawnedObject,
    uint64_t confirmedTick, uint32_t framesPerSecond) noexcept {
    if (!reservation || !runtime.plan ||
        reservation.kind != runtime.plan->kind) return false;
    if (reservation.kind == game::ObjectProductionExitKind::SpawnPoint) {
        if (!spawnedObject || reservation.slot >= runtime.spawnPointCount) {
            return false;
        }
        ObjectProductionSpawnPointRuntime& point =
            runtime.spawnPoints[reservation.slot];
        if (point.reservationToken != reservation.token) return false;
        point.reservationToken = 0;
        point.occupier = spawnedObject;
        ++runtime.revision;
        return true;
    }
    if (reservation.kind != game::ObjectProductionExitKind::Queue) {
        ++runtime.revision;
        return true;
    }
    if (runtime.activeReservationToken != reservation.token) return false;
    runtime.activeReservationToken = 0;
    if (reservation.kind == game::ObjectProductionExitKind::Queue) {
        if (runtime.initialBurstRemaining != 0) {
            --runtime.initialBurstRemaining;
        }
        const uint64_t delay = millisecondsToTicks(
            runtime.plan->exitDelayMilliseconds, framesPerSecond);
        runtime.nextExitTick = delay >
                std::numeric_limits<uint64_t>::max() - confirmedTick
            ? std::numeric_limits<uint64_t>::max()
            : confirmedTick + delay;
    }
    ++runtime.revision;
    return true;
}

[[nodiscard]] ObjectProductionSpawnIntent makeSpawnIntent(
    ObjectId producerId, const OwnerComponent& owner,
    const LogicFixedVec3& producerPosition, math::q32_32 producerYaw,
    const ObjectProductionExitComponent& exitRuntime,
    ObjectProductionExitReservation reservation,
    const ObjectProductionJob& job, uint32_t quantityIndex,
    const game::terrain::TerrainLogic& terrain, uint32_t framesPerSecond,
    const ecs::registry& registry, ecs::entity producerEntity) {
    const game::ObjectProductionExitPlan& exitPlan = *exitRuntime.plan;
    const ObjectTerrainLayerComponent* producerLayerComponent =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, producerEntity);
    const uint32_t producerLayer = producerLayerComponent
        ? producerLayerComponent->pathfindLayer
        : game::terrain::kGroundPathfindLayer;
    const auto producerLayerHeight = [&](math::q32_32 x, math::q32_32 y) {
        return math::q32_32::from_raw(
            terrain.pathfindLayerHeightRawAt(producerLayer, x.raw(), y.raw())
                .value_or(terrain.groundHeightRaw(x.raw(), y.raw())));
    };
    ObjectProductionRoutePoint exit;
    math::q32_32 exitYaw = producerYaw;
    const bool helipadProduct = game::objectHasKind(
        job.product->kindOfMask, game::ObjectKindOf::ProducedAtHelipad);
    if ((exitPlan.kind == game::ObjectProductionExitKind::SpawnPoint ||
         (exitPlan.kind == game::ObjectProductionExitKind::AirfieldParking &&
           helipadProduct)) &&
        reservation.slot < exitRuntime.spawnPointCount) {
        const ObjectProductionSpawnPointRuntime& point =
            exitRuntime.spawnPoints[reservation.slot];
        const math::q32_32 localX = reservation.hasLocalTransform
            ? reservation.localX : point.localX;
        const math::q32_32 localY = reservation.hasLocalTransform
            ? reservation.localY : point.localY;
        const math::q32_32 localZ = reservation.hasLocalTransform
            ? reservation.localZ : point.localZ;
        const math::q32_32 localYaw = reservation.hasLocalTransform
            ? reservation.localYaw : point.localYaw;
        exit = transformLocalPoint(producerPosition, producerYaw,
                                   localX, localY, localZ);
        exitYaw += localYaw;
        // HeliPark01 is a real logical bone and commonly sits above terrain
        // on the airfield model. RefCode creates the helicopter at that full
        // transform; snapping it back to the producer's ground layer places
        // it inside or below the building. Ordinary SpawnPoint exits retain
        // their pathfind-layer snap.
        if (!(exitPlan.kind ==
                  game::ObjectProductionExitKind::AirfieldParking &&
              helipadProduct)) {
            exit.z = producerLayerHeight(exit.x, exit.y);
        }
    } else {
        exit = transformLocalPoint(
            producerPosition, producerYaw, exitPlan.unitCreatePointX,
            exitPlan.unitCreatePointY, exitPlan.unitCreatePointZ);
        const math::q32_32 ground = exitPlan.kind ==
                game::ObjectProductionExitKind::Default
            ? producerLayerHeight(exit.x, exit.y)
            : math::q32_32::from_raw(
                  terrain.groundHeightRaw(exit.x.raw(), exit.y.raw()));
        const bool authoredInAir = exit.z != ground;
        if (exitPlan.kind != game::ObjectProductionExitKind::Queue ||
            !exitPlan.allowAirborneCreation || !authoredInAir) {
            exit.z = ground;
        }
    }

    const bool assignedToTargetTeam = job.targetTeam &&
        quantityIndex < job.targetTeamQuantityLimit;
    ObjectProductionSpawnIntent intent{
        .producer = producerId,
        .productionId = job.productionId,
        .quantityIndex = quantityIndex,
        .sourceSequence = job.sourceSequence,
        .owner = owner.player,
        .targetTeam = assignedToTargetTeam
            ? job.targetTeam : INVALID_OBJECT_TEAM_ID,
        .targetTeamRosterIndex = assignedToTargetTeam
            ? job.targetTeamRosterIndex : UINT32_MAX,
        .product = job.product,
        .position = {exit.x, exit.y, exit.z},
        .yawRadians = normalizeProductionYaw(exitYaw),
        .initialPathfindLayer =
            exitPlan.kind == game::ObjectProductionExitKind::Default ||
                    exitPlan.kind == game::ObjectProductionExitKind::SpawnPoint ||
                    exitPlan.kind == game::ObjectProductionExitKind::AirfieldParking ||
                    exitPlan.kind == game::ObjectProductionExitKind::FlightDeck
                ? std::optional<uint32_t>{producerLayer}
                : std::nullopt,
        .exitReservation = reservation,
        .exitDoorIndex = job.exitDoorIndex,
        .exitDoorAssigned = job.exitDoorAssigned,
    };
    switch (exitPlan.kind) {
    case game::ObjectProductionExitKind::Default:
        intent.exitRoute.push_back(naturalRallyPoint(
            producerPosition, producerYaw, exitPlan));
        break;
    case game::ObjectProductionExitKind::Queue:
        intent.exitRoute.push_back(naturalRallyPoint(
            producerPosition, producerYaw, exitPlan));
        if (!exitRuntime.rallyPoint.exists) {
            // RefCode deliberately duplicates the natural point to keep
            // multi-produced infantry from collapsing into one idle stack.
            intent.exitRoute.push_back(intent.exitRoute.front());
        }
        if (const ObjectPhysicsComponent* sourcePhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                      producerEntity)) {
            const ObjectProductionRoutePoint authored = transformLocalPoint(
                producerPosition, producerYaw, exitPlan.unitCreatePointX,
                exitPlan.unitCreatePointY, exitPlan.unitCreatePointZ);
            if (authored.z.raw() != terrain.groundHeightRaw(
                    authored.x.raw(), authored.y.raw())) {
                intent.inheritProducerKinematics = true;
                intent.producerVelocity =
                    sourcePhysics->velocityUnitsPerSecond;
            }
        }
        break;
    case game::ObjectProductionExitKind::SpawnPoint:
        intent.holdAfterSpawn = true;
        break;
    case game::ObjectProductionExitKind::SupplyCenter: {
        intent.exitRoute.push_back(transformLocalPoint(
            producerPosition, producerYaw, exitPlan.naturalRallyPointX,
            exitPlan.naturalRallyPointY, exitPlan.naturalRallyPointZ));
        intent.forceSupplyWanting = true;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, producerEntity);
        if (status && status->hasAny(game::objectStatusBit(
                          game::ObjectStatusFlag::Stealthed))) {
            const uint64_t frames = millisecondsToTicks(
                exitPlan.grantTemporaryStealthMilliseconds,
                framesPerSecond);
            intent.temporaryStealthFrames = static_cast<uint32_t>(
                std::min<uint64_t>(frames,
                    std::numeric_limits<uint32_t>::max()));
        }
        break;
    }
    case game::ObjectProductionExitKind::AirfieldParking:
    case game::ObjectProductionExitKind::FlightDeck:
        // ParkingPlace/FlightDeck own the hangar/creation-bone route. The
        // central spawn transaction binds Producer and reserves the concrete
        // aircraft slot before JetAIUpdate begins its first taxi phase.
        break;
    }
    if (assignedToTargetTeam && job.hasTargetRallyPoint) {
        intent.exitRoute.push_back({
            .x = job.targetRallyX,
            .y = job.targetRallyY,
            .z = job.targetRallyZ,
        });
    }
    if (exitRuntime.rallyPoint.exists &&
        exitPlan.kind != game::ObjectProductionExitKind::SpawnPoint) {
        intent.exitRoute.push_back({
            .x = exitRuntime.rallyPoint.x,
            .y = exitRuntime.rallyPoint.y,
            .z = exitRuntime.rallyPoint.z,
        });
    }
    return intent;
}

[[nodiscard]] container::Vector<Candidate> orderedCandidates(
    ecs::registry& registry, const ObjectLifecycle& lifecycle) {
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectProductionComponent,
                                const OwnerComponent, const TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        // `entityFromId()` deliberately hides a PendingDestroy entity from
        // ordinary game queries.  Production is one of the small number of
        // lifecycle-boundary consumers which must still see it: RefCode's
        // ProductionUpdate::onDie() cancels and refunds its queue before the
        // Object is reclaimed.  Keep the normal live lookup for arbitrary
        // stale ECS records, but admit a registered pending object exactly so
        // the update loop below can perform that one deterministic refund.
        if (!identity.id) continue;
        if (!lifecycle.entityFromId(identity.id) &&
            !lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });
    return candidates;
}

} // namespace production_detail

using namespace production_detail;

std::optional<ObjectProductionExitRoute>
ObjectProductionSystem::spawnRallyRoute(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId producer, PlayerId owner, uint32_t sourceSequence) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(producer);
    const ObjectProductionExitComponent* exit = entity
        ? ecs::try_get<ObjectProductionExitComponent>(registry, *entity)
        : nullptr;
    const ObjectFixedTransformComponent* transform = entity
        ? ecs::try_get<ObjectFixedTransformComponent>(registry, *entity)
        : nullptr;
    if (!exit || !exit->plan || !exit->plan->useSpawnRallyPoint ||
        !transform || !transform->authoritative) {
        return std::nullopt;
    }

    ObjectProductionExitRoute route{
        .producer = producer,
        .owner = owner,
        .sourceSequence = sourceSequence == 0 ? 1u : sourceSequence,
        .kind = exit->plan->kind,
    };
    route.points.push_back(naturalRallyPoint(
        transform->position, transform->yawRadians, *exit->plan));
    if (exit->rallyPoint.exists) {
        route.points.push_back({
            .x = exit->rallyPoint.x,
            .y = exit->rallyPoint.y,
            .z = exit->rallyPoint.z,
        });
    }
    return route;
}

bool setProductionDoorHoldOpen(
    ObjectProductionComponent& component, size_t doorIndex, bool holdOpen,
    uint64_t confirmedTick) noexcept {
    if (!component.plan || doorIndex >= component.doors.size() ||
        doorIndex >= component.plan->numberOfDoorAnimations) {
        return false;
    }
    ObjectProductionDoorRuntime& door = component.doors[doorIndex];
    bool changed = door.holdOpen != holdOpen;
    door.holdOpen = holdOpen;
    if (holdOpen && door.phase == ObjectProductionDoorPhase::Closed) {
        setDoorPhase(door, ObjectProductionDoorPhase::Opening,
                     confirmedTick, nextTick(confirmedTick));
        changed = true;
    }
    if (changed) ++component.revision;
    return changed;
}

int64_t calculateObjectBuildCost(
    const game::ObjectArchetype& product,
    const PlayerState& player,
    const ecs::registry& registry,
    const ObjectLifecycle& lifecycle) {
    return calculateUnitCost(product, player, registry, lifecycle);
}

bool canObjectBuildTemplate(
    ecs::registry& registry, ecs::entity builder,
    const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    const PlayerRegistry& players, PlayerId player,
    const game::ObjectArchetype& product, bool ignorePrerequisites) {
    if (!producerCanBuildUnit(registry, builder, content,
                              commandBarOverrides, players, player,
                              product, ignorePrerequisites)) {
        return false;
    }
    // BSTATUS_IGNORE_PREREQUISITES returns before both prerequisite and
    // MaxSimultaneous checks in RefCode Player::canBuild.
    return ignorePrerequisites ||
        (playerSatisfiesObjectProductionPrerequisites(
             registry, players, content, player, product) &&
         playerCanBuildMoreOfObjectType(registry, player, product));
}

bool playerSatisfiesObjectProductionPrerequisites(
    const ecs::registry& registry, const PlayerRegistry& players,
    const GameContentSnapshot& content,
    PlayerId player, const game::ObjectArchetype& product) {
    if (!player) return false;
    for (const container::String& science :
         product.templateData.prerequisiteSciences) {
        if (!players.hasScience(player, science)) return false;
    }

    const auto objects = ecs::view<const ThingTemplateComponent>(registry);
    for (const auto& alternatives :
         product.templateData.prerequisiteObjectAlternatives) {
        bool satisfied = false;
        for (const ecs::entity entity : objects) {
            // ProductionPrerequisite uses countObjectsByThingTemplate with
            // ignoreUnderConstruction=true. A foundation may already count
            // toward MaxSimultaneous, but it cannot unlock dependent content.
            if (!isCompletedAliveOwnedObject(registry, entity, player))
                continue;
            const ThingTemplateComponent& type =
                objects.template get<const ThingTemplateComponent>(entity);
            if (!type.archetype) continue;
            satisfied = std::any_of(
                alternatives.begin(), alternatives.end(),
                [&type, &content](container::StringView required) {
                    const container::SharedPtr<const game::ObjectArchetype>
                        requiredType = content.findObjectArchetype(required);
                    return requiredType &&
                        game::legacyThingTemplatesEquivalent(
                            type.archetype->templateData,
                            requiredType->templateData);
                });
            if (satisfied) break;
        }
        if (!satisfied) return false;
    }
    return true;
}

bool playerCanBuildMoreOfObjectType(
    const ecs::registry& registry, PlayerId player,
    const game::ObjectArchetype& product) noexcept {
    const uint32_t maximum = product.templateData.maxSimultaneousOfType;
    if (!player) return false;
    if (maximum == 0) return true;

    uint64_t count = 0;
    const auto objects = ecs::view<const ThingTemplateComponent>(registry);
    for (const ecs::entity entity : objects) {
        if (!isAliveOwnedObject(registry, entity, player)) continue;
        const ThingTemplateComponent& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (type.archetype && existingObjectCountsTowardLimit(
                *type.archetype, product) && ++count >= maximum) {
            return false;
        }
    }

    // RefCode counts queue entries for non-structures only. Its queue helper
    // compares the requested ThingTemplate (not LinkKey), so a linked rebuild
    // hole counts while alive but a different linked product waiting in a
    // factory does not.
    if (!game::objectHasKind(product.kindOfMask, game::ObjectKindOf::Structure)) {
        const auto producers = ecs::view<const ObjectProductionComponent>(registry);
        for (const ecs::entity entity : producers) {
            if (!isAliveOwnedObject(registry, entity, player)) continue;
            const ObjectProductionComponent& production =
                producers.template get<const ObjectProductionComponent>(entity);
            for (const ObjectProductionJob& job : production.jobs) {
                if (job.kind != ObjectProductionJobKind::Unit || !job.product ||
                    job.product->name != product.name) {
                    continue;
                }
                if (++count >= maximum) return false;
            }
        }
    }
    return true;
}

uint32_t calculateObjectBuildFrames(
    const game::ObjectArchetype& product, const PlayerState& player,
    uint32_t framesPerSecond, const EnergySimulationRules& energyRules,
    uint64_t confirmedTick) noexcept {
    return calculateUnitBuildFrames(product, player, framesPerSecond,
                                    energyRules, confirmedTick);
}

container::StringView objectProductionRejectionMessage(
    ObjectProductionRejectionReason reason) noexcept {
    switch (reason) {
    case ObjectProductionRejectionReason::None: return "";
    case ObjectProductionRejectionReason::ProducerNotFound: return "producer object was not found";
    case ObjectProductionRejectionReason::ProducerPendingDestroy: return "producer is pending destruction";
    case ObjectProductionRejectionReason::Unauthorized: return "player does not own the producer";
    case ObjectProductionRejectionReason::NotAProducer: return "object has no ProductionUpdate runtime";
    case ObjectProductionRejectionReason::ProducerDisabled:
        return "producer is disabled for its ProductionUpdate policy";
    case ObjectProductionRejectionReason::UnsupportedExit: return "producer has no supported production exit";
    case ObjectProductionRejectionReason::ProductNotFound: return "product archetype was not found";
    case ObjectProductionRejectionReason::ProductNotTrainable: return "product is not trainable";
    case ObjectProductionRejectionReason::ProductNotAvailable:
        return "product is not currently buildable or authorized by this producer";
    case ObjectProductionRejectionReason::PrerequisitesNotMet:
        return "player does not satisfy the product prerequisites";
    case ObjectProductionRejectionReason::MaximumSimultaneousReached:
        return "player has reached this product's simultaneous limit";
    case ObjectProductionRejectionReason::UpgradeNotFound: return "upgrade definition was not found";
    case ObjectProductionRejectionReason::UpgradeNotAvailable:
        return "producer cannot research this upgrade or lacks required science";
    case ObjectProductionRejectionReason::UpgradeAlreadyComplete:
        return "this upgrade is already complete for its owning scope";
    case ObjectProductionRejectionReason::UpgradeAlreadyInProgress:
        return "this upgrade is already queued or researching";
    case ObjectProductionRejectionReason::UpgradeNotInQueue:
        return "upgrade is not in this producer queue";
    case ObjectProductionRejectionReason::QueueFull: return "production queue is full";
    case ObjectProductionRejectionReason::ParkingPlacesFull:
        return "airfield parking places are full or reserved by queued aircraft";
    case ObjectProductionRejectionReason::QueueAllocationFailed:
        return "production queue could not reserve memory";
    case ObjectProductionRejectionReason::InsufficientFunds: return "insufficient funds";
    case ObjectProductionRejectionReason::ProductionIdExhausted: return "production ID space is exhausted";
    case ObjectProductionRejectionReason::ProductionIdNotFound: return "production ID is not in this queue";
    case ObjectProductionRejectionReason::InvalidRallyPoint: return "rally point is not finite";
    case ObjectProductionRejectionReason::InvalidConfirmedTick: return "command is outside the confirmed frame";
    }
    return "unknown production rejection";
}

} // namespace engine
