#include "game/session/query/InGameCommandProjection.h"

#include "core/container/string_utils.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/structure/ObjectOvercharge.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/player/PlayerRegistry.h"
#include "game/selection/LocalSelectionState.h"
#include "game/session/core/GameSession.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/GameSessionEconomyQueryPort.h"

#include <algorithm>
#include <limits>

namespace engine::session_query {
namespace {

using game::CommandButtonKind;
using game::CommandButtonOption;

[[nodiscard]] uint16_t narrowU16(size_t value) noexcept {
    return static_cast<uint16_t>(std::min<size_t>(
        value, std::numeric_limits<uint16_t>::max()));
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t product = static_cast<uint64_t>(milliseconds) *
        framesPerSecond;
    return std::max<uint64_t>(1, (product + 999u) / 1000u);
}

[[nodiscard]] uint16_t readyPermille(
    uint64_t remaining, uint64_t total) noexcept {
    if (remaining == 0) return 1000;
    if (total == 0 || remaining >= total) return 0;
    return static_cast<uint16_t>(
        ((total - remaining) * 1000u) / total);
}

void reject(InGameCommandSlotAvailability& value,
            InGameCommandAvailabilityReason reason,
            bool visible = true) noexcept {
    value.visible = visible;
    value.enabled = false;
    value.active = false;
    value.reason = reason;
}

[[nodiscard]] bool containsExact(
    container::Span<const container::String> values,
    container::StringView value) noexcept {
    return std::any_of(values.begin(), values.end(),
        [value](const container::String& candidate) {
            return candidate == value;
        });
}

[[nodiscard]] bool kindOfContains(
    container::StringView values, container::StringView expected) noexcept {
    size_t cursor = 0;
    while (cursor < values.size()) {
        while (cursor < values.size() &&
               (values[cursor] == ' ' || values[cursor] == '\t' ||
                values[cursor] == ',' || values[cursor] == '|')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < values.size() && values[cursor] != ' ' &&
               values[cursor] != '\t' && values[cursor] != ',' &&
               values[cursor] != '|') {
            ++cursor;
        }
        if (begin != cursor && container::asciiEqualIgnoreCase(
                values.substr(begin, cursor - begin), expected)) {
            return true;
        }
    }
    return false;
}

// RefCode BattlePlanUpdate::getCommandOption maps the desired Strategy Center
// plan onto these three option bits, and ControlBarCommand.cpp tests it against
// the button's options to report COMMAND_ACTIVE. They are battle-plan identity,
// never a weapon slot: the slot lives in the authored WeaponSlot field, which
// CommandButtonStore now compiles into descriptor.weaponSlot.
[[nodiscard]] game::ObjectBattlePlanStatus battlePlanFromCommandOptions(
    uint32_t options) noexcept {
    if (game::hasCommandButtonOption(
            options, CommandButtonOption::OptionOne)) {
        return game::ObjectBattlePlanStatus::Bombardment;
    }
    if (game::hasCommandButtonOption(
            options, CommandButtonOption::OptionTwo)) {
        return game::ObjectBattlePlanStatus::HoldTheLine;
    }
    if (game::hasCommandButtonOption(
            options, CommandButtonOption::OptionThree)) {
        return game::ObjectBattlePlanStatus::SearchAndDestroy;
    }
    return game::ObjectBattlePlanStatus::None;
}

void projectQueue(const engine::ObjectProductionComponent* production,
                  InGameCommandQueueProjection& queue) noexcept {
    if (!production || !production->plan) return;
    queue.revision = production->revision;
    queue.count = narrowU16(production->jobs.size());
    queue.capacity = narrowU16(production->plan->maxQueueEntries);
    if (production->jobs.empty()) return;

    const engine::ObjectProductionJob& head = production->jobs.front();
    queue.headProductionId = head.productionId;
    if (head.constructionComplete) {
        queue.headProgressPermille = 1000;
    } else if (head.lastRequiredFrames != 0) {
        queue.headProgressPermille = static_cast<uint16_t>(
            std::min<uint64_t>(1000,
                static_cast<uint64_t>(head.framesUnderConstruction) * 1000u /
                    head.lastRequiredFrames));
    }
}

[[nodiscard]] bool hasPendingBuildTask(
    const engine::ObjectBuilderComponent& builder) noexcept {
    for (const engine::ObjectBuilderRuntime& runtime : builder.runtimes) {
        for (const engine::ObjectBuilderTask& task : runtime.taskSlots) {
            if (task.kind == engine::ObjectBuilderTaskKind::Build &&
                task.target) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool hasQueuedObjectUpgrade(
    const engine::ObjectProductionComponent& production,
    engine::UpgradeContentId upgrade) noexcept {
    return std::any_of(production.jobs.begin(), production.jobs.end(),
        [upgrade](const engine::ObjectProductionJob& job) {
            return job.kind == engine::ObjectProductionJobKind::ObjectUpgrade &&
                job.upgrade == upgrade;
        });
}

[[nodiscard]] bool productBuildabilityAdmits(
    const InGameCommandQuerySource& source,
    const engine::PlayerState& player,
    const game::ObjectArchetype& product,
    bool& ignorePrerequisites) noexcept {
    ignorePrerequisites = false;
    const game::ObjectBuildabilityStatus status =
        source.effectiveScriptObjectBuildability(
            product.templateData.name).value_or(
                product.templateData.buildability);
    switch (status) {
    case game::ObjectBuildabilityStatus::Yes:
        return true;
    case game::ObjectBuildabilityStatus::IgnorePrerequisites:
        ignorePrerequisites = true;
        return true;
    case game::ObjectBuildabilityStatus::OnlyByAi:
        return player.controller == engine::PlayerControllerKind::Ai;
    case game::ObjectBuildabilityStatus::No:
        return false;
    }
    return false;
}

[[nodiscard]] const engine::SpecialPowerDefinition* resolveSpecialPower(
    const engine::GameContentSnapshot& content,
    const game::CommandButtonTemplate& button) noexcept {
    return button.specialPower.empty()
        ? nullptr : content.findSpecialPower(button.specialPower);
}

[[nodiscard]] const engine::ObjectSpecialPowerRuntime* findSpecialPowerRuntime(
    const engine::ObjectSpecialPowerComponent* powers,
    engine::SpecialPowerContentId content) noexcept {
    if (!powers || !content) return nullptr;
    const auto found = std::find_if(
        powers->instances.begin(), powers->instances.end(),
        [content](const engine::ObjectSpecialPowerRuntime& runtime) {
            return runtime.content == content;
        });
    return found == powers->instances.end() ? nullptr : &*found;
}

void projectCooldown(InGameCommandSlotAvailability& value,
                     uint64_t remaining, uint64_t total) noexcept {
    value.cooldown.remainingTicks = remaining;
    value.cooldown.totalTicks = total;
    value.cooldown.readyPermille = readyPermille(remaining, total);
    if (remaining != 0) {
        reject(value, InGameCommandAvailabilityReason::Cooldown);
    }
}

} // namespace

bool sameInGameCommandAvailabilityState(
    const InGameCommandSlotAvailability& lhs,
    const InGameCommandSlotAvailability& rhs) noexcept {
    return lhs.visible == rhs.visible &&
        lhs.enabled == rhs.enabled &&
        lhs.active == rhs.active &&
        lhs.reason == rhs.reason &&
        lhs.cost == rhs.cost &&
        lhs.cooldown == rhs.cooldown &&
        lhs.queue == rhs.queue;
}

InGameCommandSlotAvailability InGameCommandQuerySource::evaluateAvailability(
    const GameSessionCommandQueryPort& commands,
    const GameSessionEconomyQueryPort& economy,
    uint64_t confirmedTick, uint32_t logicFramesPerSecond,
    const engine::selection::LocalSelectionState& selection,
    engine::ObjectId actor,
    const game::CommandButtonTemplate& button,
    bool sourceVisible,
    engine::ObjectId commandTarget,
    engine::PlayerId evaluatingPlayer) const {
    InGameCommandSlotAvailability value;
    value.visible = sourceVisible;
    value.enabled = sourceVisible;
    value.reason = sourceVisible
        ? InGameCommandAvailabilityReason::None
        : InGameCommandAvailabilityReason::MissingButton;
    if (!sourceVisible) return value;
    if (!button.descriptor.recognized()) {
        reject(value, InGameCommandAvailabilityReason::UnsupportedCommand);
        return value;
    }
    if (!button.descriptor.requiredReferencesPresent ||
        game::hasCommandButtonOption(
            button.descriptor.options, CommandButtonOption::ScriptOnly)) {
        reject(value,
               InGameCommandAvailabilityReason::MissingContentReference,
               false);
        return value;
    }
    if (!actor || !selection.contains(actor)) {
        reject(value, InGameCommandAvailabilityReason::ActorUnavailable,
               false);
        return value;
    }
    if (selection.selected().size() > 1 &&
        !game::hasCommandButtonOption(
            button.descriptor.options,
            CommandButtonOption::OkForMultiSelect)) {
        reject(value, InGameCommandAvailabilityReason::MissingCapability,
               false);
        return value;
    }

    const InGameCommandQuerySource& source = *this;
    const std::optional<ecs::entity> entity = source.entityFromId(actor);
    if (!entity) {
        reject(value, InGameCommandAvailabilityReason::ActorUnavailable,
               false);
        return value;
    }
    const engine::ThingTemplateComponent* type =
        ecs::try_get<engine::ThingTemplateComponent>(
            source.registry(), *entity);
    const engine::PlayerState* player = evaluatingPlayer
        ? source.player(evaluatingPlayer) : source.localPlayer();
    const std::optional<engine::PlayerId> owner =
        source.ownerOf(actor);
    if (!type || !type->archetype || !player ||
        !player->isCommandPlayer() ||
        owner != std::optional<engine::PlayerId>{player->id}) {
        reject(value, InGameCommandAvailabilityReason::UnauthorizedActor,
               false);
        return value;
    }
    const bool buttonSciencesSatisfied = std::all_of(
        button.sciences.begin(), button.sciences.end(),
        [&source, player](container::StringView science) {
            return science.empty() ||
                container::asciiEqualIgnoreCase(science, "None") ||
                source.playerHasScience(player->id, science);
        });
    const engine::ObjectKindOfComponent* kinds =
        ecs::try_get<engine::ObjectKindOfComponent>(
            source.registry(), *entity);
    const bool ignoredInGui = kinds && game::objectHasKind(
        kinds->mask, game::ObjectKindOf::IgnoredInGui);
    const engine::ObjectStatusComponent* objectStatus =
        ecs::try_get<engine::ObjectStatusComponent>(
            source.registry(), *entity);
    const bool constructionCancel = button.descriptor.kind ==
        CommandButtonKind::DozerConstructCancel;
    const engine::ObjectHealthComponent* health =
        ecs::try_get<engine::ObjectHealthComponent>(
            source.registry(), *entity);
    if (ignoredInGui ||
        (objectStatus && objectStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Sold))) ||
        (health && health->effectivelyDead) ||
        ecs::try_get<engine::ObjectSaleComponent>(
            source.registry(), *entity)) {
        reject(value, InGameCommandAvailabilityReason::ActorUnavailable,
               false);
        return value;
    }

    // RefCode switches a foundation to CP_UNDER_CONSTRUCTION. Its normal
    // CommandSet is not merely disabled: it is absent, and the dedicated
    // context exposes only Command_CancelConstruction (plus non-command rally
    // presentation). Never let a foundation inherit production, upgrades,
    // powers or Sell from the finished building's authored CommandSet.
    if (objectStatus && objectStatus->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction)) &&
        button.descriptor.kind != CommandButtonKind::DozerConstructCancel) {
        reject(value, InGameCommandAvailabilityReason::MissingCapability,
               false);
        return value;
    }

    if (objectStatus && objectStatus->singleUseCommandUsed) {
        reject(value, InGameCommandAvailabilityReason::SingleUseConsumed);
        return value;
    }

    const engine::ObjectDisabledMask disabled =
        engine::objectDisabledMask(
            source.registry(), *entity, confirmedTick);
    const auto disabledBy = [disabled](engine::ObjectDisabledReason reason) {
        return (disabled & engine::objectDisabledBit(reason)) != 0;
    };
    if (!constructionCancel &&
        (disabledBy(engine::ObjectDisabledReason::ScriptDisabled) ||
        disabledBy(engine::ObjectDisabledReason::ScriptUnderpowered))) {
        reject(value, InGameCommandAvailabilityReason::ScriptDisabled,
               false);
        return value;
    }
    if (!constructionCancel &&
        disabledBy(engine::ObjectDisabledReason::Unmanned)) {
        reject(value, InGameCommandAvailabilityReason::Unmanned, false);
        return value;
    }

    const engine::ObjectProductionComponent* production =
        ecs::try_get<engine::ObjectProductionComponent>(
            source.registry(), *entity);
    projectQueue(production, value.queue);

    const bool underpowered =
        disabledBy(engine::ObjectDisabledReason::Underpowered);
    const engine::ObjectDisabledMask underpoweredMask =
        engine::objectDisabledBit(
            engine::ObjectDisabledReason::Underpowered);
    const engine::ObjectDisabledMask otherDisabled =
        disabled & ~underpoweredMask;
    if (underpowered &&
        !game::hasCommandButtonOption(
            button.descriptor.options,
            CommandButtonOption::IgnoresUnderpowered) &&
        !game::commandButtonWorksWhileDisabled(button.descriptor.kind)) {
        reject(value, InGameCommandAvailabilityReason::Underpowered);
        return value;
    }
    if (otherDisabled != 0 &&
        !game::commandButtonWorksWhileDisabled(button.descriptor.kind)) {
        reject(value, InGameCommandAvailabilityReason::Disabled);
        return value;
    }

    if (game::hasCommandButtonOption(
            button.descriptor.options,
            CommandButtonOption::MustBeStopped)) {
        const engine::ObjectLocomotionComponent* locomotion =
            ecs::try_get<engine::ObjectLocomotionComponent>(
                source.registry(), *entity);
        if (locomotion &&
            (locomotion->state == engine::ObjectLocomotionState::Moving ||
             locomotion->hasActiveMove)) {
            reject(value,
                   InGameCommandAvailabilityReason::MustBeStopped);
            return value;
        }
    }
    if (production && !production->jobs.empty() &&
        game::hasCommandButtonOption(
            button.descriptor.options,
            CommandButtonOption::NotQueueable)) {
        reject(value, InGameCommandAvailabilityReason::QueueBusy);
        return value;
    }

    const engine::GameContentSnapshot& content = source.contentSnapshot();
    const game::ObjectArchetype* product = nullptr;
    container::SharedPtr<const game::ObjectArchetype> productHandle;
    if (button.descriptor.costSource ==
            game::CommandButtonCostSource::ObjectTemplate) {
        productHandle = content.findObjectArchetype(button.object);
        product = productHandle.get();
        if (!product) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return value;
        }
        value.cost = std::max<int64_t>(0,
            economy.objectBuildCost(*product, *player));
    }

    const engine::UpgradeDefinition* upgrade = nullptr;
    if (button.descriptor.costSource ==
            game::CommandButtonCostSource::Upgrade) {
        const engine::UpgradeCatalog* upgrades = content.upgradeCatalog();
        upgrade = upgrades ? upgrades->find(button.upgrade) : nullptr;
        if (!upgrade) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return value;
        }
        value.cost = std::max<int64_t>(0, upgrade->buildCost);
    }

    const engine::ScienceDefinition* science = nullptr;
    if (button.descriptor.costSource ==
            game::CommandButtonCostSource::Science) {
        const engine::ScienceCatalog* sciences = content.scienceCatalog();
        science = sciences ? sciences->find(button.science) : nullptr;
        if (!science) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return value;
        }
        value.cost = std::max<int64_t>(0, science->purchasePointCost);
    }

    if (game::hasCommandButtonOption(
            button.descriptor.options, CommandButtonOption::NeedUpgrade)) {
        const engine::UpgradeCatalog* upgrades = content.upgradeCatalog();
        const engine::UpgradeDefinition* required =
            upgrades ? upgrades->find(button.upgrade) : nullptr;
        if (!required) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return value;
        }
        const bool present = required->type ==
                engine::UpgradeDefinitionType::Player
            ? source.playerHasUpgradeComplete(
                  player->id, required->id)
            : engine::ObjectUpgradeSystem{}.hasObjectUpgrade(
                  source.registry(), *entity, required->id);
        if (!present) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingPrerequisiteUpgrade);
            return value;
        }
    }

    const engine::SpecialPowerDefinition* specialPower =
        resolveSpecialPower(content, button);
    if (game::hasCommandButtonOption(
            button.descriptor.options,
            CommandButtonOption::NeedSpecialPowerScience)) {
        if (!specialPower) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return value;
        }
        if (!specialPower->requiredScience.empty() &&
            !source.playerHasScience(
                player->id, specialPower->requiredScience)) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingPrerequisiteScience,
                   false);
            return value;
        }
    }

    const auto applySpecialPowerRuntimeGate = [&]() -> bool {
        if (!specialPower) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return false;
        }
        const engine::ObjectSpecialPowerComponent* powers =
            ecs::try_get<engine::ObjectSpecialPowerComponent>(
                source.registry(), *entity);
        const engine::ObjectSpecialPowerRuntime* runtime =
            findSpecialPowerRuntime(powers, specialPower->id);
        if (!runtime || !powers || !powers->plan) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return false;
        }
        const game::ObjectSpecialPowerRule* rule = nullptr;
        const size_t count = std::min(
            powers->instances.size(), powers->plan->rules.size());
        for (size_t index = 0; index < count; ++index) {
            if (powers->instances[index].content == specialPower->id) {
                rule = &powers->plan->rules[index];
                break;
            }
        }
        if (!rule || rule->scriptedOnly ||
            rule->kind == game::ObjectSpecialPowerKind::Unsupported ||
            rule->kind == game::ObjectSpecialPowerKind::CashBounty) {
            reject(value,
                   InGameCommandAvailabilityReason::UnsupportedCommand);
            return false;
        }
        if (runtime->pausedCount != 0) {
            reject(value, InGameCommandAvailabilityReason::Disabled);
            return false;
        }
        if (!specialPower->requiredScience.empty() &&
            !source.playerHasScience(
                player->id, specialPower->requiredScience)) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingPrerequisiteScience);
            return false;
        }
        const uint64_t remaining = runtime->readyTick > confirmedTick
            ? runtime->readyTick - confirmedTick : 0;
        projectCooldown(
            value, remaining,
            millisecondsToTicks(
                specialPower->reloadTimeMilliseconds,
                logicFramesPerSecond));
        return value.enabled;
    };

    switch (button.descriptor.kind) {
    case CommandButtonKind::DozerConstructCancel: {
        const engine::ObjectStatusComponent* status =
            ecs::try_get<engine::ObjectStatusComponent>(
                source.registry(), *entity);
        if (!status || !status->hasAny(game::objectStatusBit(
                           game::ObjectStatusFlag::UnderConstruction))) {
            reject(value, InGameCommandAvailabilityReason::MissingCapability,
                   false);
            return value;
        }
        break;
    }
    case CommandButtonKind::DozerConstruct:
    case CommandButtonKind::UnitBuild:
    case CommandButtonKind::SpecialPowerConstruct:
    case CommandButtonKind::SpecialPowerConstructFromShortcut: {
        if ((button.descriptor.kind == CommandButtonKind::SpecialPowerConstruct ||
             button.descriptor.kind == CommandButtonKind::SpecialPowerConstructFromShortcut) &&
            !applySpecialPowerRuntimeGate()) {
            return value;
        }
        if (!product) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingContentReference);
            return value;
        }
        bool ignorePrerequisites = false;
        if (!productBuildabilityAdmits(
                source, *player, *product, ignorePrerequisites)) {
            reject(value,
                   InGameCommandAvailabilityReason::ProductUnavailable,
                   false);
            return value;
        }
        const bool structure = kindOfContains(
            product->templateData.kindOf, "STRUCTURE");
        if ((structure &&
             !player->constructionPolicy.baseConstructionEnabled) ||
            (!structure &&
             !player->constructionPolicy.unitConstructionEnabled)) {
            reject(value,
                   InGameCommandAvailabilityReason::ProductUnavailable);
            return value;
        }
        if (button.descriptor.kind == CommandButtonKind::DozerConstruct) {
            const engine::ObjectBuilderComponent* builder =
                ecs::try_get<engine::ObjectBuilderComponent>(
                    source.registry(), *entity);
            if (!builder || !builder->plan) {
                reject(value,
                       InGameCommandAvailabilityReason::MissingCapability);
                return value;
            }
            if (hasPendingBuildTask(*builder)) {
                reject(value,
                       InGameCommandAvailabilityReason::QueueBusy);
                return value;
            }
        } else if (!production || !production->plan) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        } else {
            if (production->jobs.size() >=
                production->plan->maxQueueEntries) {
                reject(value,
                       InGameCommandAvailabilityReason::QueueFull);
                return value;
            }
            if (!engine::ObjectProductionSystem{}
                     .hasQueueCapacityForProduct(
                         source.registry(), *entity, *production,
                         *product)) {
                reject(value,
                       InGameCommandAvailabilityReason::ParkingPlacesFull);
                return value;
            }
            if (button.descriptor.kind == CommandButtonKind::UnitBuild &&
                !production->exitPlan) {
                reject(value,
                       InGameCommandAvailabilityReason::MissingCapability);
                return value;
            }
        }
        if (!ignorePrerequisites &&
            !buttonSciencesSatisfied) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingPrerequisiteScience);
            return value;
        }
        if (!ignorePrerequisites &&
            !source.playerSatisfiesProductionPrerequisites(
                player->id, *product)) {
            reject(value,
                   InGameCommandAvailabilityReason::PrerequisitesNotMet);
            return value;
        }
        if (!ignorePrerequisites &&
            !engine::playerCanBuildMoreOfObjectType(
                source.registry(), player->id, *product)) {
            reject(value,
                   InGameCommandAvailabilityReason::MaximumSimultaneousReached);
            return value;
        }
        if (value.cost > player->cash) {
            reject(value,
                   InGameCommandAvailabilityReason::InsufficientFunds);
            return value;
        }
        break;
    }

    case CommandButtonKind::PlayerUpgrade:
    case CommandButtonKind::ObjectUpgrade: {
        if (!upgrade || !production || !production->plan) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        const bool playerUpgrade = button.descriptor.kind ==
            CommandButtonKind::PlayerUpgrade;
        if (playerUpgrade != (upgrade->type ==
                engine::UpgradeDefinitionType::Player)) {
            reject(value,
                   InGameCommandAvailabilityReason::ProductUnavailable);
            return value;
        }
        if (production->jobs.size() >=
            production->plan->maxQueueEntries) {
            reject(value, InGameCommandAvailabilityReason::QueueFull);
            return value;
        }
        if (!buttonSciencesSatisfied) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingPrerequisiteScience);
            return value;
        }
        if (playerUpgrade) {
            if (source.playerHasUpgradeComplete(
                    player->id, upgrade->id)) {
                reject(value,
                       InGameCommandAvailabilityReason::AlreadyComplete);
                return value;
            }
            if (source.playerHasUpgradeInProgress(
                    player->id, upgrade->id)) {
                reject(value,
                       InGameCommandAvailabilityReason::AlreadyInProgress);
                return value;
            }
        } else {
            const engine::ObjectUpgradeSystem upgradeSystem;
            if (upgradeSystem.hasObjectUpgrade(
                    source.registry(), *entity, upgrade->id)) {
                reject(value,
                       InGameCommandAvailabilityReason::AlreadyComplete);
                return value;
            }
            if (hasQueuedObjectUpgrade(*production, upgrade->id)) {
                reject(value,
                       InGameCommandAvailabilityReason::AlreadyInProgress);
                return value;
            }
            // A ProductionUpdate with MaxQueueEntries > 1 can already hold a
            // different object upgrade. UpgradeMux ConflictsWith applies to
            // the prospective completed set, so include every queued local
            // upgrade when testing the next choice. This prevents mutually
            // exclusive add-ons (Overlord/Helix/drone families, including
            // modded equivalents) from entering the queue together before
            // the first one reaches completion.
            engine::UpgradeMask completedWithQueued =
                player->upgrades.completed;
            for (const engine::ObjectProductionJob& job : production->jobs) {
                if (job.kind ==
                        engine::ObjectProductionJobKind::ObjectUpgrade &&
                    job.upgrade) {
                    engine::upgradeMaskSet(
                        completedWithQueued, job.upgrade);
                }
            }
            if (!upgradeSystem.canReceiveObjectUpgrade(
                    source.registry(), *entity,
                    completedWithQueued,
                    upgrade->id)) {
                reject(value,
                       InGameCommandAvailabilityReason::ProductUnavailable);
                return value;
            }
        }
        if (value.cost > player->cash) {
            reject(value,
                   InGameCommandAvailabilityReason::InsufficientFunds);
            return value;
        }
        break;
    }

    case CommandButtonKind::PurchaseScience:
        if (!science || !science->grantable ||
            containsExact(player->sciences.hidden, science->name)) {
            reject(value,
                   InGameCommandAvailabilityReason::ProductUnavailable,
                   false);
            return value;
        }
        if (containsExact(player->sciences.known, science->name)) {
            reject(value,
                   InGameCommandAvailabilityReason::AlreadyComplete);
            return value;
        }
        if (containsExact(player->sciences.disabled, science->name)) {
            reject(value,
                   InGameCommandAvailabilityReason::ProductUnavailable);
            return value;
        }
        for (const container::String& prerequisite :
             science->prerequisiteSciences) {
            if (!source.playerHasScience(player->id, prerequisite)) {
                reject(value,
                       InGameCommandAvailabilityReason::MissingPrerequisiteScience);
                return value;
            }
        }
        if (science->purchasePointCost <= 0 ||
            science->purchasePointCost >
                player->sciences.purchasePoints) {
            reject(value,
                   InGameCommandAvailabilityReason::InsufficientFunds);
            return value;
        }
        break;

    case CommandButtonKind::FireWeapon:
    case CommandButtonKind::SwitchWeapon: {
        const engine::ObjectWeaponComponent* weapons =
            ecs::try_get<engine::ObjectWeaponComponent>(
                source.registry(), *entity);
        if (!weapons || !weapons->activeWeaponSetIndex ||
            *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        // The authored WeaponSlot field is the slot, matching RefCode's
        // getWeaponInWeaponSlot(command->getWeaponSlot()). Reading
        // OPTION_ONE/TWO/THREE instead pinned every FIRE_WEAPON/SWITCH_WEAPON
        // button to PRIMARY, so sibling slots showed the wrong reload clock and
        // no warhead/detonation-mode button ever reported itself active.
        const size_t slotIndex =
            static_cast<size_t>(button.descriptor.weaponSlot);
        const engine::ObjectWeaponSetRuntime& set =
            weapons->sets[*weapons->activeWeaponSetIndex];
        if (slotIndex >= set.slots.size()) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        const engine::ObjectWeaponSlotRuntime& slot = set.slots[slotIndex];
        const game::WeaponTemplate* weapon = content.findWeapon(slot.content);
        if (!weapon) {
            const engine::ObjectCombatProfileComponent* combat =
                ecs::try_get<engine::ObjectCombatProfileComponent>(
                    source.registry(), *entity);
            const game::WeaponSetConditionMask mineClearing =
                game::weaponSetConditionBit(
                    game::WeaponSetCondition::MineClearingDetail);
            const bool canEnterMineClearingWeaponSet =
                game::hasCommandButtonOption(
                    button.descriptor.options,
                    CommandButtonOption::UsesMineClearingWeaponset) &&
                combat &&
                (combat->weaponConditions & mineClearing) == 0;
            if (canEnterMineClearingWeaponSet) {
                break;
            }
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        value.active = button.descriptor.kind ==
                CommandButtonKind::SwitchWeapon &&
            weapons->currentSlot &&
            static_cast<size_t>(*weapons->currentSlot) == slotIndex;
        const uint64_t readyTick = std::max({
            slot.nextReadyTick, slot.reloadCompleteTick,
            set.sharedReloadCompleteTick});
        const uint64_t remaining = readyTick > confirmedTick
            ? readyTick - confirmedTick : 0;
        const uint64_t total = slot.reloadCompleteTick > confirmedTick ||
                set.sharedReloadCompleteTick > confirmedTick
            ? millisecondsToTicks(
                  weapon->clipReloadTimeMilliseconds,
                  logicFramesPerSecond)
            : millisecondsToTicks(
                  weapon->maximumDelayBetweenShotsMilliseconds,
                  logicFramesPerSecond);
        value.cooldown = {
            .remainingTicks = remaining,
            .totalTicks = total,
            .readyPermille = readyPermille(remaining, total),
        };
        if (button.descriptor.kind == CommandButtonKind::FireWeapon &&
            remaining != 0) {
            reject(value, InGameCommandAvailabilityReason::Cooldown);
            return value;
        }
        break;
    }

    case CommandButtonKind::SpecialPower:
    case CommandButtonKind::SpecialPowerFromShortcut: {
        if (!applySpecialPowerRuntimeGate()) return value;
        // RefCode's SPECIAL_CHANGE_BATTLE_PLANS branch reports COMMAND_ACTIVE
        // once the power is ready and BattlePlanUpdate::getCommandOption()
        // intersects the button's options, which is what keeps the selected
        // Strategy Center plan drawn pressed. getCommandOption() reports the
        // desired plan, so compare against BattlePlanRuntime::desired rather
        // than the packing/unpacking `current`.
        if (specialPower->specialPowerType ==
                game::SpecialPowerType::ChangeBattlePlans) {
            const game::ObjectBattlePlanStatus desired =
                battlePlanFromCommandOptions(button.descriptor.options);
            const engine::ObjectTacticalComponent* tactical =
                ecs::try_get<engine::ObjectTacticalComponent>(
                    source.registry(), *entity);
            value.active = tactical &&
                desired != game::ObjectBattlePlanStatus::None &&
                std::any_of(
                    tactical->battlePlans.begin(),
                    tactical->battlePlans.end(),
                    [desired](const engine::ObjectBattlePlanRuntime& plan) {
                        return plan.desired == desired;
                    });
        }
        break;
    }

    case CommandButtonKind::ToggleOvercharge: {
        const engine::ObjectOverchargeComponent* overcharge =
            ecs::try_get<engine::ObjectOverchargeComponent>(
                source.registry(), *entity);
        if (!overcharge || !overcharge->plan) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        value.active = std::any_of(
            overcharge->instances.begin(), overcharge->instances.end(),
            [](const engine::ObjectOverchargeRuntime& runtime) {
                return runtime.active;
            });
        break;
    }

    case CommandButtonKind::ExitContainer: {
        const std::optional<ecs::entity> passenger =
            source.entityFromId(commandTarget);
        if (!commandTarget || !passenger ||
            !commands.canExitPassengerThrough(
                actor, commandTarget) ||
            source.ownerOf(commandTarget) != player->id) {
            reject(value, InGameCommandAvailabilityReason::NoPassengers);
            return value;
        }
        break;
    }

    case CommandButtonKind::ExecuteRailedTransport: {
        const engine::ObjectRailedTransportRuntimeComponent* railed =
            ecs::try_get<engine::ObjectRailedTransportRuntimeComponent>(
                source.registry(), *entity);
        const bool ready = railed && railed->plan &&
            !railed->plan->railedTransportAi.empty() &&
            std::any_of(
                railed->instances.begin(), railed->instances.end(),
                [](const engine::ObjectRailedTransportRuntime& runtime) {
                    return runtime.dockOpen && !runtime.inTransit &&
                        !runtime.loadingOrUnloading &&
                        !runtime.executeRequested;
                });
        if (!ready) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        break;
    }

    case CommandButtonKind::Evacuate:
    case CommandButtonKind::CombatDrop: {
        const engine::ObjectContainmentComponent* containment =
            ecs::try_get<engine::ObjectContainmentComponent>(
                source.registry(), *entity);
        if (!containment || containment->objects.empty()) {
            reject(value, InGameCommandAvailabilityReason::NoPassengers);
            return value;
        }
        break;
    }

    case CommandButtonKind::SetRallyPoint:
        if (!ecs::try_get<engine::ObjectProductionExitComponent>(
                source.registry(), *entity)) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        break;

    case CommandButtonKind::Sell: {
        const engine::ObjectScriptPanelPolicyComponent* policy =
            ecs::try_get<engine::ObjectScriptPanelPolicyComponent>(
                source.registry(), *entity);
        if ((policy && policy->unsellable)) {
            reject(value, InGameCommandAvailabilityReason::Unsellable,
                   false);
            return value;
        }
        if (health && health->subdued) {
            reject(value, InGameCommandAvailabilityReason::Disabled);
            return value;
        }
        break;
    }

    case CommandButtonKind::Waypoints:
        // Retail GUI_COMMAND_WAYPOINTS has no click-side gameplay message;
        // waypoint queuing is the BEGIN/END_WAYPOINTS input modifier, which
        // InputCoordinator already implements. Do not advertise this authored
        // placeholder as a separately executable backend. Only Waypoints is
        // click-inert: ATTACK_MOVE/GUARD*/STOP each reach a live consumer and
        // must stay on the shared AIUpdate gate below.
        reject(value, InGameCommandAvailabilityReason::MissingCapability);
        return value;

    case CommandButtonKind::AttackMove:
    case CommandButtonKind::Guard:
    case CommandButtonKind::GuardWithoutPursuit:
    case CommandButtonKind::GuardFlyingUnitsOnly:
    case CommandButtonKind::HackInternet:
    case CommandButtonKind::HijackVehicle:
    case CommandButtonKind::ConvertToCarBomb:
    case CommandButtonKind::SabotageBuilding:
    {
        if (!type->archetype->hasAiUpdate) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        const bool attackMove = button.descriptor.kind ==
            CommandButtonKind::AttackMove;
        if ((button.descriptor.kind == CommandButtonKind::Guard ||
             button.descriptor.kind ==
                 CommandButtonKind::GuardWithoutPursuit ||
             button.descriptor.kind ==
                 CommandButtonKind::GuardFlyingUnitsOnly || attackMove) &&
            (!commands.hasOrderCapability(
                 actor, engine::ai::ObjectAIOrderCapability::MoveStop) ||
             !commands.hasOrderCapability(
                 actor, engine::ai::ObjectAIOrderCapability::Attack))) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        break;
    }

    case CommandButtonKind::Stop:
        if (!type->archetype->hasAiUpdate) {
            reject(value,
                   InGameCommandAvailabilityReason::MissingCapability);
            return value;
        }
        if (game::hasCommandButtonOption(
                button.descriptor.options,
                CommandButtonOption::OptionOne)) {
            const engine::ObjectTacticalComponent* tactical =
                ecs::try_get<engine::ObjectTacticalComponent>(
                    source.registry(), *entity);
            const bool activeBombardment = tactical && std::any_of(
                tactical->battlePlans.begin(),
                tactical->battlePlans.end(),
                [](const engine::ObjectBattlePlanRuntime& plan) {
                    return plan.transition ==
                               engine::ObjectBattlePlanTransition::Active &&
                           plan.current ==
                               game::ObjectBattlePlanStatus::Bombardment;
                });
            if (!activeBombardment) {
                reject(value,
                       InGameCommandAvailabilityReason::Disabled,
                       false);
                return value;
            }
        }
        break;

    case CommandButtonKind::Unknown:
    case CommandButtonKind::None:
        reject(value,
               InGameCommandAvailabilityReason::UnsupportedCommand);
        return value;

    default:
        break;
    }

    value.reason = InGameCommandAvailabilityReason::None;
    value.enabled = true;
    return value;
}

InGameCommandSlotAvailability evaluateInGameCommandAvailability(
    const InGameCommandEvaluationContext& context,
    const engine::selection::LocalSelectionState& selection,
    engine::ObjectId actor,
    const game::CommandButtonTemplate& button,
    bool sourceVisible,
    engine::ObjectId commandTarget) {
    return context.source.evaluateAvailability(
        context.commands, context.economy, context.confirmedTick,
        context.logicFramesPerSecond, selection, actor, button,
        sourceVisible, commandTarget, context.player);
}

InGameCommandSlotAvailability evaluateInGameCommandAvailability(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    engine::ObjectId actor,
    const game::CommandButtonTemplate& button,
    bool sourceVisible,
    engine::ObjectId commandTarget) {
    const InGameCommandQuerySource source =
        inGameCommandQuerySource(session);
    const GameSessionCommandQueryPort commands = session.commandQuery();
    const GameSessionEconomyQueryPort economy = session.economyQuery();
    return evaluateInGameCommandAvailability(
        {
            .source = source,
            .commands = commands,
            .economy = economy,
            .confirmedTick = session.confirmedTick(),
            .logicFramesPerSecond = session.logicFramesPerSecond(),
        },
        selection, actor, button, sourceVisible, commandTarget);
}

} // namespace engine::session_query
