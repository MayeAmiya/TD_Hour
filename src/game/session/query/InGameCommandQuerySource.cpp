#include "game/session/query/InGameCommandQuerySource.h"

#include "game/command/CommandBarOverrides.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/session/core/GameSession.h"
#include "game/script/presentation/ScriptCommandBarPresentationConsumer.h"
#include "core/container/string_utils.h"

#include <algorithm>
#include <limits>

namespace engine::session_query {

const ecs::registry& InGameCommandQuerySource::registry() const noexcept {
    return *m_registry;
}

const PlayerState* InGameCommandQuerySource::localPlayer() const noexcept {
    return m_players->localPlayer();
}

const PlayerState* InGameCommandQuerySource::player(
    PlayerId id) const noexcept {
    return id ? m_players->get(id) : nullptr;
}

std::optional<PlayerId> InGameCommandQuerySource::ownerOf(
    ObjectId object) const noexcept {
    return m_ownership->ownerOf(object);
}

container::Span<const ObjectId> InGameCommandQuerySource::ownedObjects(
    PlayerId player) const noexcept {
    return m_ownership->objects(player);
}

bool InGameCommandQuerySource::playerHasScience(
    PlayerId player, container::StringView science) const noexcept {
    return m_players->hasScience(player, science);
}

bool InGameCommandQuerySource::playerHasUpgradeComplete(
    PlayerId player, UpgradeContentId upgrade) const noexcept {
    return m_players->hasUpgradeComplete(player, upgrade);
}

bool InGameCommandQuerySource::playerHasUpgradeInProgress(
    PlayerId player, UpgradeContentId upgrade) const noexcept {
    return m_players->hasUpgradeInProgress(player, upgrade);
}

bool InGameCommandQuerySource::playerSatisfiesProductionPrerequisites(
    PlayerId player, const game::ObjectArchetype& product) const {
    return engine::playerSatisfiesObjectProductionPrerequisites(
        *m_registry, *m_players, *m_content, player, product);
}

const game::CommandButtonTemplate*
InGameCommandQuerySource::findCommandButton(
    container::StringView name) const noexcept {
    return m_content->findCommandButton(name);
}

container::SharedPtr<const game::ObjectArchetype>
InGameCommandQuerySource::findObjectArchetype(
    container::StringView name) const {
    return m_content->findObjectArchetype(name);
}

const SpecialPowerDefinition* InGameCommandQuerySource::findSpecialPower(
    container::StringView name) const noexcept {
    return m_content->findSpecialPower(name);
}

const game::WeaponTemplate* InGameCommandQuerySource::findWeapon(
    game::WeaponContentId weapon) const noexcept {
    return m_content->findWeapon(weapon);
}

bool InGameCommandQuerySource::isCommandBarActor(
    ObjectId object, bool multiSelection) const noexcept {
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    if (!entity) return false;
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(*m_registry, *entity);
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(*m_registry, *entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(*m_registry, *entity);
    if (!identity || identity->id != object || !type || !type->archetype ||
        !visual || visual->modelAsset.empty() || visual->hidden ||
        (health && health->effectivelyDead)) {
        return false;
    }
    const container::StringView name = type->name;
    const auto frozen = m_content->findObjectArchetype(name);
    if (name.empty() || !frozen ||
        frozen.get() != type->archetype.get() ||
        type->archetype->templateData.name != name) {
        return false;
    }
    if (!multiSelection) return true;
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(*m_registry, *entity);
    if (kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::IgnoredInGui)) {
        return false;
    }
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(*m_registry, *entity);
    return !status || !status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Sold));
}

container::String InGameCommandQuerySource::objectButtonImage(
    ObjectId object) const {
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    const ThingTemplateComponent* type = entity
        ? ecs::try_get<ThingTemplateComponent>(*m_registry, *entity)
        : nullptr;
    return type && type->archetype
        ? type->archetype->templateData.buttonImage : container::String{};
}

InGameSelectionPresentationReadModel
InGameCommandQuerySource::objectSelectionPresentation(ObjectId object) const {
    InGameSelectionPresentationReadModel result;
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    const ThingTemplateComponent* type = entity
        ? ecs::try_get<ThingTemplateComponent>(*m_registry, *entity)
        : nullptr;
    if (!type || !type->archetype) return result;

    const game::ThingTemplate& thing = type->archetype->templateData;
    result.portraitImage = thing.selectedPortraitImage;
    const UpgradeCatalog* catalog = m_content->upgradeCatalog();
    const ObjectUpgradeInventoryComponent* inventory = entity
        ? ecs::try_get<ObjectUpgradeInventoryComponent>(*m_registry, *entity)
        : nullptr;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(*m_registry, *entity)
        : nullptr;
    for (size_t index = 0; index < thing.upgradeCameos.size(); ++index) {
        const container::String& authored = thing.upgradeCameos[index];
        if (authored.empty() || container::asciiEqualIgnoreCase(
                authored, "none") || !catalog) {
            continue;
        }
        const UpgradeDefinition* upgrade = catalog->find(authored);
        if (!upgrade || upgrade->buttonImage.empty()) continue;
        InGameSelectionUpgradeReadModel& output = result.upgrades[index];
        output.buttonImage = upgrade->buttonImage;
        output.visible = true;
        output.complete =
            (inventory && upgradeMaskTest(inventory->completed, upgrade->id)) ||
            (owner && m_players->hasUpgradeComplete(owner->player, upgrade->id));
    }
    return result;
}

InGameConstructionReadModel InGameCommandQuerySource::objectConstruction(
    ObjectId object) const noexcept {
    InGameConstructionReadModel result;
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    if (!entity) return result;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(*m_registry, *entity);
    result.underConstruction = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
    if (!result.underConstruction) return result;
    const ObjectConstructionSiteComponent* site =
        ecs::try_get<ObjectConstructionSiteComponent>(*m_registry, *entity);
    if (!site || site->requiredFrames == 0) return result;
    result.progressPermille = static_cast<uint16_t>(std::min<uint64_t>(
        1000u, static_cast<uint64_t>(site->completedFrames) * 1000u /
            site->requiredFrames));
    return result;
}

InGameBuilderConstructionReadModel
InGameCommandQuerySource::objectBuilderConstruction(
    ObjectId object) const noexcept {
    InGameBuilderConstructionReadModel result{.builder = object};
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    const ObjectBuilderComponent* builder = entity
        ? ecs::try_get<ObjectBuilderComponent>(*m_registry, *entity)
        : nullptr;
    if (!builder || builder->runtimes.empty()) return result;

    result.isBuilder = true;
    if (const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(*m_registry, *entity)) {
        result.queuedBuildCount = static_cast<uint16_t>(std::min<size_t>(
            std::count_if(
                queue->orders.begin(), queue->orders.end(),
                [](const ObjectOrderIntent& order) {
                    return order.kind == ObjectOrderKind::Build &&
                        !order.targetObject;
                }),
            std::numeric_limits<uint16_t>::max()));
    }
    uint64_t newestIssuedTick = 0;
    for (const ObjectBuilderRuntime& runtime : builder->runtimes) {
        for (const ObjectBuilderTask& task : runtime.taskSlots) {
            if (task.kind != ObjectBuilderTaskKind::Build || !task.target)
                continue;
            if (!result.pendingBuild || task.issuedTick > newestIssuedTick ||
                (task.issuedTick == newestIssuedTick &&
                 task.sourceSequence > result.sourceSequence)) {
                result.pendingBuild = true;
                newestIssuedTick = task.issuedTick;
                result.sourceSequence = task.sourceSequence;
            }
        }
    }
    return result;
}

InGameOrderWaypointReadModel InGameCommandQuerySource::orderWaypoint(
    ObjectId actor, uint32_t sourceSequence) const {
    InGameOrderWaypointReadModel result;
    if (!actor || sourceSequence == 0) return result;
    const std::optional<ecs::entity> entity = m_objects->entityFromId(actor);
    const ObjectOrderQueueComponent* queue = entity
        ? ecs::try_get<ObjectOrderQueueComponent>(*m_registry, *entity)
        : nullptr;
    if (!queue) return result;
    const auto found = std::find_if(
        queue->orders.begin(), queue->orders.end(),
        [&](const ObjectOrderIntent& order) {
            return order.source == ObjectOrderSource::Player &&
                order.sourceSequence == sourceSequence &&
                (order.kind == ObjectOrderKind::Move ||
                 order.kind == ObjectOrderKind::Attack ||
                 order.kind == ObjectOrderKind::Build ||
                 order.kind == ObjectOrderKind::SpecialPower ||
                 order.kind == ObjectOrderKind::CommandButton ||
                 (order.kind == ObjectOrderKind::TacticalAttack &&
                  order.tacticalAttackSubtype ==
                      ObjectTacticalAttackSubtype::Guard));
        });
    if (found == queue->orders.end()) return result;
    result.exists = true;
    result.kind = found->kind == ObjectOrderKind::Build
        ? selection::LocalOrderWaypointKind::Build
        : found->kind == ObjectOrderKind::Attack
            ? selection::LocalOrderWaypointKind::Attack
        : found->kind == ObjectOrderKind::TacticalAttack
            ? selection::LocalOrderWaypointKind::Guard
        : found->kind == ObjectOrderKind::SpecialPower ||
                found->kind == ObjectOrderKind::CommandButton
            ? selection::LocalOrderWaypointKind::Ability
            : found->attackMove
                ? selection::LocalOrderWaypointKind::AttackMove
                : selection::LocalOrderWaypointKind::Move;
    if (found->kind == ObjectOrderKind::Build) {
        result.objectType = found->contentName;
        const container::SharedPtr<const game::ObjectArchetype> product =
            m_content->findObjectArchetype(found->contentName);
        if (product) {
            result.portraitImage =
                product->templateData.selectedPortraitImage;
        }
    }
    return result;
}

uint64_t InGameCommandQuerySource::commandSetRevision(
    ObjectId object) const noexcept {
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    const ObjectCommandSetOverrideComponent* commandSet = entity
        ? ecs::try_get<ObjectCommandSetOverrideComponent>(*m_registry, *entity)
        : nullptr;
    return commandSet ? commandSet->revision : 0;
}

InGameProductionReadModel InGameCommandQuerySource::productionQueue(
    ObjectId object, size_t maximumItems) const {
    InGameProductionReadModel result;
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    const ObjectProductionComponent* production = entity
        ? ecs::try_get<ObjectProductionComponent>(*m_registry, *entity)
        : nullptr;
    if (!production || !production->plan || production->jobs.empty())
        return result;
    result.revision = production->revision;
    result.capacity = production->plan->maxQueueEntries;
    result.totalCount = production->jobs.size();
    result.items.reserve(std::min(maximumItems, production->jobs.size()));
    const UpgradeCatalog* upgrades = m_content->upgradeCatalog();
    const auto sameQueueProduct = [](const ObjectProductionJob& left,
                                     const ObjectProductionJob& right) {
        if (left.kind != right.kind) return false;
        if (left.kind == ObjectProductionJobKind::Unit) {
            return left.product && right.product &&
                left.product.get() == right.product.get();
        }
        return left.upgrade && left.upgrade == right.upgrade;
    };
    const ObjectProductionJob* previous = nullptr;
    for (const ObjectProductionJob& job : production->jobs) {
        if (previous && !result.items.empty() &&
            sameQueueProduct(*previous, job)) {
            InGameProductionReadItem& grouped = result.items.back();
            if (grouped.queuedCount != std::numeric_limits<uint16_t>::max()) {
                ++grouped.queuedCount;
            }
            // Cancel the newest real entry in this consecutive run. Keeping
            // the first item's progress while removing from the tail prevents
            // a click on A3 from resetting the currently producing A.
            grouped.productionId = job.productionId;
            if (grouped.cancellationProductionIdCount <
                InGameProductionReadItem::kMaximumBatchCancellationCount) {
                grouped.cancellationProductionIds[
                    grouped.cancellationProductionIdCount++] =
                        job.productionId;
            } else {
                std::move(
                    grouped.cancellationProductionIds.begin() + 1,
                    grouped.cancellationProductionIds.end(),
                    grouped.cancellationProductionIds.begin());
                grouped.cancellationProductionIds.back() = job.productionId;
            }
            previous = &job;
            continue;
        }
        if (result.items.size() >= maximumItems) break;
        InGameProductionReadItem item;
        item.productionId = job.productionId;
        item.cancellationProductionIds[0] = job.productionId;
        item.cancellationProductionIdCount = 1;
        item.kind = job.kind == ObjectProductionJobKind::Unit
            ? InGameProductionReadKind::Unit
            : job.kind == ObjectProductionJobKind::PlayerUpgrade
                ? InGameProductionReadKind::PlayerUpgrade
                : InGameProductionReadKind::ObjectUpgrade;
        item.upgradeName = job.upgradeName;
        if (job.constructionComplete) {
            item.progressPermille = 1000;
        } else if (job.lastRequiredFrames != 0) {
            item.progressPermille = static_cast<uint16_t>(std::min<uint64_t>(
                1000, static_cast<uint64_t>(job.framesUnderConstruction) *
                    1000u / job.lastRequiredFrames));
        }
        if (job.kind == ObjectProductionJobKind::Unit && job.product) {
            item.buttonImage = job.product->templateData.buttonImage;
            item.textLabel = job.product->templateData.displayName;
            if (item.textLabel.empty())
                item.textLabel = job.product->templateData.name;
        } else if (const UpgradeDefinition* upgrade = upgrades
                       ? upgrades->find(job.upgrade) : nullptr) {
            item.upgradeName = upgrade->name;
            item.buttonImage = upgrade->buttonImage;
            item.textLabel = upgrade->displayNameLabel.empty()
                ? upgrade->name : upgrade->displayNameLabel;
        }
        result.items.push_back(std::move(item));
        previous = &job;
    }
    return result;
}

InGameBeaconReadModel InGameCommandQuerySource::beacon(
    ObjectId object) const {
    InGameBeaconReadModel result;
    const std::optional<ecs::entity> entity = m_objects->entityFromId(object);
    if (!entity) return result;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *entity);
    result.isBeacon = type && type->archetype &&
        type->archetype->techBuildingPlan &&
        !type->archetype->techBuildingPlan->beacons.empty();
    if (!result.isBeacon) return result;
    const ObjectDrawableCaptionComponent* caption =
        ecs::try_get<ObjectDrawableCaptionComponent>(*m_registry, *entity);
    if (caption) {
        result.caption = caption->text;
        result.revision = caption->revision;
    }
    return result;
}

container::Vector<InGamePublicTimerPowerReadModel>
InGameCommandQuerySource::publicTimerSpecialPowers() const {
    container::Vector<InGamePublicTimerPowerReadModel> result;
    const SpecialPowerCatalog* catalog =
        m_content->specialPowerCatalog();
    if (!catalog) return result;
    for (const PlayerId player : m_players->activePlayerIds()) {
        for (const ObjectId object : m_ownership->objects(player)) {
            const std::optional<ecs::entity> entity =
                m_objects->entityFromId(object);
            if (!entity) continue;
            const ObjectSpecialPowerComponent* powers =
                ecs::try_get<ObjectSpecialPowerComponent>(
                    *m_registry, *entity);
            if (!powers || powers->instances.empty()) continue;
            // RefCode gates registration on KINDOF_STRUCTURE precisely so a
            // scripted bomber carrying the same SpecialPower template cannot
            // publish a public countdown.
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(*m_registry, *entity);
            if (!kinds ||
                !game::objectHasKind(kinds->mask,
                                     game::ObjectKindOf::Structure)) {
                continue;
            }
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(*m_registry, *entity);
            const bool underConstruction = status &&
                status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction));
            for (const ObjectSpecialPowerRuntime& runtime :
                 powers->instances) {
                const SpecialPowerDefinition* definition =
                    catalog->find(runtime.content);
                if (!definition || !definition->publicTimer ||
                    !definition->sharedSyncedTimer) {
                    continue;
                }
                result.push_back({
                    .specialPower = definition->name,
                    .owner = player,
                    .source = object,
                    .readyTick = runtime.readyTick,
                    .paused = runtime.pausedCount != 0,
                    .hiddenByScience =
                        !definition->requiredScience.empty() &&
                        !m_players->hasScience(player,
                                               definition->requiredScience),
                    .underConstruction = underConstruction,
                });
            }
        }
    }
    return result;
}

bool InGameCommandQuerySource::productionQueueActionCurrent(
    ObjectId producer, uint32_t productionId, InGameProductionReadKind kind,
    container::StringView upgradeName) const noexcept {
    const std::optional<ecs::entity> entity = m_objects->entityFromId(producer);
    const ObjectProductionComponent* production = entity
        ? ecs::try_get<ObjectProductionComponent>(*m_registry, *entity)
        : nullptr;
    if (!production) return false;
    // A production id identifies one concrete job.  `revision` is a UI
    // refresh stamp and changes for unrelated enqueue/completion events; it
    // must not invalidate cancellation of a still-present job.
    return std::any_of(
        production->jobs.begin(), production->jobs.end(),
        [&](const ObjectProductionJob& candidate) {
            if (candidate.productionId != productionId) return false;
            switch (kind) {
            case InGameProductionReadKind::Unit:
                return candidate.kind == ObjectProductionJobKind::Unit;
            case InGameProductionReadKind::PlayerUpgrade:
                return candidate.kind ==
                        ObjectProductionJobKind::PlayerUpgrade &&
                    candidate.upgradeName == upgradeName;
            case InGameProductionReadKind::ObjectUpgrade:
                return candidate.kind ==
                        ObjectProductionJobKind::ObjectUpgrade &&
                    candidate.upgradeName == upgradeName;
            }
            return false;
        });
}

math::q32_32 InGameCommandQuerySource::pendingCommandRadius(
    ObjectId actor, const game::CommandButtonTemplate& button) const {
    const container::StringView kind = button.radiusCursorType;
    if (kind.empty() || container::asciiEqualIgnoreCase(kind, "NONE"))
        return {};
    if (!button.specialPower.empty()) {
        if (const SpecialPowerDefinition* definition =
                m_content->findSpecialPower(button.specialPower);
            definition &&
            definition->radiusCursorRadius > math::q32_32{}) {
            return definition->radiusCursorRadius;
        }
    }
    const std::optional<ecs::entity> entity = m_objects->entityFromId(actor);
    if (!entity) return {};
    if (container::asciiEqualIgnoreCase(kind, "GUARD_AREA"))
        return effectiveObjectVisionRangeFixed(*m_registry, *entity);
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(*m_registry, *entity);
    if (!weapons || weapons->sets.empty()) return {};
    game::WeaponSlot authoredSlot = game::WeaponSlot::Primary;
    for (auto field = button.fields.rbegin(); field != button.fields.rend();
         ++field) {
        if (!container::asciiEqualIgnoreCase(field->first, "WeaponSlot"))
            continue;
        authoredSlot = container::asciiEqualIgnoreCase(
                field->second, "SECONDARY")
            ? game::WeaponSlot::Secondary
            : container::asciiEqualIgnoreCase(field->second, "TERTIARY")
                ? game::WeaponSlot::Tertiary : game::WeaponSlot::Primary;
        break;
    }
    const size_t setIndex = weapons->activeWeaponSetIndex &&
            *weapons->activeWeaponSetIndex < weapons->sets.size()
        ? *weapons->activeWeaponSetIndex : 0u;
    const ObjectWeaponSlotRuntime& slot = weapons->sets[setIndex].slots[
        static_cast<size_t>(authoredSlot)];
    const game::WeaponTemplate* weapon = m_content->findWeapon(slot.content);
    if (!weapon) return {};
    if (container::asciiEqualIgnoreCase(kind, "ATTACK_DAMAGE_AREA"))
        return math::q32_32::max(
            math::q32_32{}, weapon->fixed.primaryDamageRadius);
    if (container::asciiEqualIgnoreCase(kind, "ATTACK_SCATTER_AREA"))
        return math::q32_32::max(
            math::q32_32{}, weapon->fixed.scatterRadius +
                weapon->fixed.scatterTargetScalar);
    if (container::asciiEqualIgnoreCase(kind, "ATTACK_CONTINUE_AREA") ||
        container::asciiEqualIgnoreCase(kind, "CLEARMINES")) {
        return math::q32_32::max(
            math::q32_32{}, weapon->fixed.continueAttackRange);
    }
    return {};
}

container::String InGameCommandQuerySource::specialPowerInitiateSound(
    ObjectId actor, const game::CommandButtonTemplate& button) const {
    if (!actor || button.specialPower.empty()) return {};
    const SpecialPowerDefinition* definition =
        m_content->findSpecialPower(button.specialPower);
    if (!definition) return {};
    const std::optional<ecs::entity> entity = m_objects->entityFromId(actor);
    const ObjectSpecialPowerComponent* powers = entity
        ? ecs::try_get<ObjectSpecialPowerComponent>(*m_registry, *entity)
        : nullptr;
    if (!powers || !powers->plan) return {};

    // CommandXlat asks the SpecialPowerModuleInterface for this cue.  The
    // runtime and frozen rule arrays share authored order, so resolving by
    // the exact SpecialPower content id also preserves multiple abilities on
    // one object without falling back to a broad type-based guess.
    const size_t count = std::min(
        powers->instances.size(), powers->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        if (powers->instances[index].content != definition->id) continue;
        return powers->plan->rules[index].initiateSound;
    }
    return {};
}

bool InGameCommandQuerySource::synchronizeCommandBar(
    script::ScriptCommandBarPresentationConsumer& consumer,
    ObjectId object,
    container::Span<const container::String> effectiveButtons) const {
    script::ScriptCommandBarPresentationConsumer::ButtonNameArray names{};
    const size_t count = std::min(names.size(), effectiveButtons.size());
    for (size_t index = 0; index < count; ++index)
        names[index] = effectiveButtons[index];
    return consumer.synchronizeEffective(
        *m_content, objectTypeName(object), effectiveCommandSetName(object),
        names);
}

const GameContentSnapshot&
InGameCommandQuerySource::contentSnapshot() const noexcept {
    return *m_content;
}

game::CommandBarOverrideMutationStamp
InGameCommandQuerySource::commandBarMutation() const noexcept {
    return m_commandBarOverrides->lastMutation();
}

std::optional<ecs::entity> InGameCommandQuerySource::entityFromId(
    ObjectId object) const noexcept {
    return m_objects->entityFromId(object);
}

container::StringView InGameCommandQuerySource::objectTypeName(
    ObjectId object) const noexcept {
    const std::optional<ecs::entity> entity = entityFromId(object);
    if (!entity) return {};
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *entity);
    return type && type->archetype
        ? container::StringView{type->archetype->templateData.name}
        : container::StringView{};
}

container::StringView InGameCommandQuerySource::effectiveCommandSetName(
    ObjectId object) const noexcept {
    const std::optional<ecs::entity> entity = entityFromId(object);
    return entity
        ? engine::effectiveObjectCommandSetName(*m_registry, *entity)
        : container::StringView{};
}

container::StringView
InGameCommandQuerySource::effectiveObjectCommandBarButton(
    ObjectId object, size_t slot) const {
    if (slot >= game::COMMAND_SET_SLOT_COUNT) return {};
    const container::StringView commandSetName =
        effectiveCommandSetName(object);
    const game::CommandSetTemplate* commandSet =
        m_content->findCommandSet(commandSetName);
    if (!commandSet) return {};
    return m_commandBarOverrides->effectiveButtonName(
        commandSet->name, slot, commandSet->commands[slot]);
}

std::optional<game::ObjectBuildabilityStatus>
InGameCommandQuerySource::effectiveScriptObjectBuildability(
    container::StringView objectType) const noexcept {
    const container::SharedPtr<const game::ObjectArchetype> archetype =
        m_content->findObjectArchetype(objectType);
    if (!archetype) return std::nullopt;
    const auto found = m_buildabilityOverrides->find(
        archetype->templateData.name);
    return found == m_buildabilityOverrides->end()
        ? std::optional<game::ObjectBuildabilityStatus>{
              archetype->templateData.buildability}
        : std::optional<game::ObjectBuildabilityStatus>{found->second};
}

InGameCommandQuerySource inGameCommandQuerySource(
    const GameSession& session) noexcept {
    return session.makeInGameCommandQuerySource();
}

} // namespace engine::session_query
