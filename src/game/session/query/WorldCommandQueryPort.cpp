#include "game/session/query/WorldCommandQueryPort.h"

#include "core/container/string_utils.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/runtime/ObjectSimulationDomains.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/combat/ObjectWeaponTargetPolicy.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/selection/PendingWorldCommandMode.h"

#include <algorithm>

namespace engine::selection {
namespace {

[[nodiscard]] bool hasKindOf(const ObjectKindOfComponent* kinds,
                             game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool usableEntity(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ObjectId object, ecs::entity& entity) noexcept {
    if (!object || objects.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> resolved = objects.entityFromId(object);
    if (!resolved) return false;
    const ObjectLifecycleComponent* life =
        ecs::try_get<ObjectLifecycleComponent>(registry, *resolved);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *resolved);
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, *resolved);
    if ((life && life->phase != ObjectLifecyclePhase::Alive) ||
        (health && health->effectivelyDead) || (map && map->offMap)) {
        return false;
    }
    entity = *resolved;
    return true;
}

[[nodiscard]] bool hasEnterContainmentInterface(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, entity);
    if (!runtime || !runtime->plan) return false;
    return std::any_of(
        runtime->plan->rules.begin(), runtime->plan->rules.end(),
        [](const ObjectContainmentRule& rule) noexcept {
            switch (rule.kind) {
            case ObjectContainmentKind::Transport:
            case ObjectContainmentKind::Garrison:
            case ObjectContainmentKind::Tunnel:
            case ObjectContainmentKind::Cave:
            case ObjectContainmentKind::Heal:
                return rule.containMax != 0;
            default:
                return false;
            }
        });
}

// GarrisonContain is not an ordinary transport admission.  A player may take
// an empty hostile/neutral building, but may not enter one already occupied
// by another owner.  The confirmed Garrison transaction enforces this before
// it reserves capacity; mirror the same fact here so contextual targeting
// does not advertise an Enter cursor for a command that it will reject.
[[nodiscard]] bool garrisonHasForeignOccupant(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ecs::entity container, ecs::entity actor) noexcept {
    const OwnerComponent* actorOwner =
        ecs::try_get<OwnerComponent>(registry, actor);
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, container);
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, container);
    if (!runtime || !runtime->plan || !contents) return false;

    for (size_t ruleIndex = 0; ruleIndex < runtime->plan->rules.size();
         ++ruleIndex) {
        if (runtime->plan->rules[ruleIndex].kind !=
            ObjectContainmentKind::Garrison) {
            continue;
        }
        for (const ObjectContainedObjectRecord& record : contents->objects) {
            const std::optional<ecs::entity> passenger =
                objects.entityFromId(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (!passenger || !edge ||
                edge->containmentRuleIndex != ruleIndex) {
                continue;
            }
            const OwnerComponent* passengerOwner =
                ecs::try_get<OwnerComponent>(registry, *passenger);
            if (!actorOwner || !passengerOwner ||
                actorOwner->player != passengerOwner->player) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool weaponRuntimeCanAttack(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::NoAttack))) {
        return false;
    }
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }
    const ObjectWeaponSetRuntime& activeSet =
        weapons->sets[*weapons->activeWeaponSetIndex];
    return std::any_of(
        activeSet.slots.begin(), activeSet.slots.end(),
        [](const ObjectWeaponSlotRuntime& slot) {
            return static_cast<bool>(slot.content);
        });
}

[[nodiscard]] const game::WeaponTemplate* resolvedOrderWeapon(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content,
    std::optional<uint8_t> requestedSlot) noexcept {
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return nullptr;
    }
    const uint8_t slotValue = requestedSlot.value_or(
        static_cast<uint8_t>(weapons->currentSlot.value_or(
            game::WeaponSlot::Primary)));
    const size_t slotIndex = static_cast<size_t>(slotValue);
    const ObjectWeaponSetRuntime& activeSet =
        weapons->sets[*weapons->activeWeaponSetIndex];
    if (slotIndex >= activeSet.slots.size()) return nullptr;
    return content.findWeapon(activeSet.slots[slotIndex].content);
}

[[nodiscard]] bool spawnChildCanAttack(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    const ai::ObjectAIRuntime* objectAI, ObjectId spawner) noexcept {
    ecs::entity spawnerEntity = ecs::null;
    if (!usableEntity(registry, objects, spawner, spawnerEntity)) {
        return false;
    }
    const ObjectSpawnSlaveComponent* component =
        ecs::try_get<ObjectSpawnSlaveComponent>(registry, spawnerEntity);
    if (!component || !component->plan) return false;

    for (const ObjectSpawnRuntime& runtime : component->spawns) {
        for (const ObjectId child : runtime.children) {
            ecs::entity childEntity = ecs::null;
            if (!objectAI || !objectAI->hasOrderCapability(
                    child, ai::ObjectAIOrderCapability::Attack) ||
                !usableEntity(registry, objects, child, childEntity)) {
                continue;
            }
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(registry, childEntity);
            if (status && status->hasAny(
                    game::objectStatusBit(game::ObjectStatusFlag::NoAttack))) {
                continue;
            }
            if (status && status->hasAny(
                    game::objectStatusBit(game::ObjectStatusFlag::CanAttack))) {
                return true;
            }
            if (hasKindOf(
                    ecs::try_get<ObjectKindOfComponent>(registry, childEntity),
                    game::ObjectKindOf::CanAttack)) {
                return true;
            }
            const ObjectCombatProfileComponent* combat =
                ecs::try_get<ObjectCombatProfileComponent>(
                    registry, childEntity);
            if (combat && combat->profile &&
                combat->profile->hasAnyWeapons()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool WorldCommandQueryPort::isCommandPlayer(PlayerId player) const noexcept {
    const PlayerState* state = m_players->get(player);
    return state && state->isCommandPlayer();
}

bool WorldCommandQueryPort::isControlledLiveObject(
    PlayerId player, ObjectId object) const noexcept {
    return m_ownership->ownerOf(object) == player &&
        m_objects->entityFromId(object).has_value();
}

bool WorldCommandQueryPort::relationAllowed(
    PendingWorldTargetRelation allowed, PlayerId localPlayer,
    ObjectId target) const noexcept {
    const uint8_t mask = static_cast<uint8_t>(allowed);
    if (mask == 0) return true;
    const std::optional<PlayerId> owner = m_ownership->ownerOf(target);
    const PlayerRelationship relationship = !owner
        ? PlayerRelationship::Neutral
        : *owner == localPlayer
            ? PlayerRelationship::Allies
            : m_players->relationship(localPlayer, *owner);
    PendingWorldTargetRelation required = PendingWorldTargetRelation::Neutral;
    if (relationship == PlayerRelationship::Enemies) {
        required = PendingWorldTargetRelation::Enemy;
    } else if (relationship == PlayerRelationship::Allies) {
        required = PendingWorldTargetRelation::Ally;
    }
    return (mask & static_cast<uint8_t>(required)) != 0;
}

bool WorldCommandQueryPort::isAttackableTarget(ObjectId target) const noexcept {
    ecs::entity entity = ecs::null;
    if (!usableEntity(*m_registry, *m_objects, target, entity)) return false;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(*m_registry, entity);
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(*m_registry, entity);
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(*m_registry, entity);
    return health && health->acceptsDamage &&
        !hasKindOf(
            ecs::try_get<ObjectKindOfComponent>(*m_registry, entity),
            game::ObjectKindOf::Unattackable) &&
        !(status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Masked))) &&
        !(contained && contained->enclosing);
}

bool WorldCommandQueryPort::isAttackableTargetForPlayer(
    PlayerId player, ObjectId target) const noexcept {
    if (!player || !isAttackableTarget(target)) return false;
    const std::optional<PlayerId> owner = m_ownership->ownerOf(target);
    if (!owner) return true;
    if (*owner == player) return false;
    return m_players->relationship(player, *owner) !=
        PlayerRelationship::Allies;
}

WorldContextTargetAction WorldCommandQueryPort::contextualTarget(
    PlayerId localPlayer, ObjectId target) const noexcept {
    ecs::entity entity = ecs::null;
    if (!usableEntity(*m_registry, *m_objects, target, entity)) {
        return WorldContextTargetAction::Ground;
    }
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(*m_registry, entity);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(*m_registry, entity);
    if (owner && owner->player == localPlayer &&
        hasKindOf(kinds, game::ObjectKindOf::Mine)) {
        return WorldContextTargetAction::Ground;
    }
    // RefCode rejects a KINDOF_UNATTACKABLE victim at the top of
    // WeaponSet::getAbleToAttackSpecificObject, so such an object never yields
    // the Attack context action. It may still expose a containment entry.
    const bool unattackable =
        hasKindOf(kinds, game::ObjectKindOf::Unattackable);
    const ObjectScriptPanelPolicyComponent* panel =
        ecs::try_get<ObjectScriptPanelPolicyComponent>(*m_registry, entity);
    if (!unattackable && owner && owner->player &&
        owner->player != localPlayer) {
        const PlayerRelationship relationship =
            m_players->relationship(localPlayer, owner->player);
        if (relationship == PlayerRelationship::Enemies) {
            return WorldContextTargetAction::Attack;
        }
        if (relationship == PlayerRelationship::Neutral &&
            ((panel && panel->playerTargetable) ||
             hasKindOf(kinds, game::ObjectKindOf::Mine))) {
            return WorldContextTargetAction::Attack;
        }
    }
    return hasEnterContainmentInterface(*m_registry, entity)
        ? WorldContextTargetAction::Enter
        : WorldContextTargetAction::Reserved;
}

bool WorldCommandQueryPort::actorCanMove(ObjectId actor) const noexcept {
    ecs::entity entity = ecs::null;
    return usableEntity(*m_registry, *m_objects, actor, entity) &&
        ecs::try_get<ObjectLocomotionComponent>(*m_registry, entity) != nullptr;
}

bool WorldCommandQueryPort::actorCanEnterContainer(
    ObjectId actor, ObjectId container) const noexcept {
    ecs::entity actorEntity = ecs::null;
    ecs::entity containerEntity = ecs::null;
    if (!m_containment || !actorCanMove(actor) ||
        !usableEntity(*m_registry, *m_objects, actor, actorEntity) ||
        !usableEntity(*m_registry, *m_objects, container, containerEntity)) {
        return false;
    }
    if (!m_containment->canContain(
        *m_registry, *m_objects,
        {.kind = ObjectContainmentRequestKind::Attach,
         .container = container,
         .object = actor},
        m_players)) {
        return false;
    }
    return !garrisonHasForeignOccupant(
        *m_registry, *m_objects, containerEntity, actorEntity);
}

bool WorldCommandQueryPort::actorCanAttack(ObjectId actor) const noexcept {
    ecs::entity entity = ecs::null;
    if (!usableEntity(*m_registry, *m_objects, actor, entity)) return false;
    const bool directConsumer = m_objectAI &&
        m_objectAI->hasOrderCapability(
            actor, ai::ObjectAIOrderCapability::Attack);
    if (directConsumer && weaponRuntimeCanAttack(*m_registry, entity))
        return true;
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(*m_registry, entity);
    return hasKindOf(kinds, game::ObjectKindOf::SpawnsAreTheWeapons) &&
        spawnChildCanAttack(*m_registry, *m_objects, m_objectAI, actor);
}

bool WorldCommandQueryPort::actorCanAttackTarget(
    ObjectId actor, ObjectId target) const noexcept {
    ecs::entity actorEntity = ecs::null;
    ecs::entity targetEntity = ecs::null;
    if (!usableEntity(*m_registry, *m_objects, actor, actorEntity) ||
        !usableEntity(*m_registry, *m_objects, target, targetEntity) ||
        !m_content) {
        return false;
    }
    const bool directConsumer = m_objectAI &&
        m_objectAI->hasOrderCapability(
            actor, ai::ObjectAIOrderCapability::Attack);
    if (directConsumer && weaponRuntimeCanAttackTarget(
            *m_registry, actorEntity, targetEntity, *m_content)) {
        return true;
    }
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(*m_registry, actorEntity);
    if (!hasKindOf(kinds, game::ObjectKindOf::SpawnsAreTheWeapons))
        return false;
    const ObjectSpawnSlaveComponent* component =
        ecs::try_get<ObjectSpawnSlaveComponent>(*m_registry, actorEntity);
    if (!component) return false;
    for (const ObjectSpawnRuntime& runtime : component->spawns) {
        for (const ObjectId child : runtime.children) {
            ecs::entity childEntity = ecs::null;
            if (m_objectAI && m_objectAI->hasOrderCapability(
                    child, ai::ObjectAIOrderCapability::Attack) &&
                usableEntity(*m_registry, *m_objects, child, childEntity) &&
                weaponRuntimeCanAttackTarget(
                    *m_registry, childEntity, targetEntity, *m_content)) {
                return true;
            }
        }
    }
    return false;
}

bool WorldCommandQueryPort::actorCanForceAttackTarget(
    ObjectId actor, ObjectId target) const noexcept {
    // WeaponSet::getAbleToAttackSpecificObject rejects self before applying
    // its forced-attack relationship exception. The target-level predicate
    // retains MASKED, UNATTACKABLE, damageability and enclosing-containment
    // gates; actorCanAttackTarget then proves an actual compatible consumer.
    return actor && target && actor != target && isAttackableTarget(target) &&
        actorCanAttackTarget(actor, target);
}

WorldCommandWeaponVoiceKind WorldCommandQueryPort::weaponOrderVoice(
    ObjectId actor, std::optional<uint8_t> requestedWeaponSlot,
    ObjectId target, bool commandButtonWeapon) const noexcept {
    ecs::entity actorEntity = ecs::null;
    if (!m_content ||
        !usableEntity(*m_registry, *m_objects, actor, actorEntity)) {
        return WorldCommandWeaponVoiceKind::None;
    }
    const game::WeaponTemplate* weapon = resolvedOrderWeapon(
        *m_registry, actorEntity, *m_content, requestedWeaponSlot);
    if (!weapon) return WorldCommandWeaponVoiceKind::None;

    ecs::entity targetEntity = ecs::null;
    const bool objectTarget = target &&
        usableEntity(*m_registry, *m_objects, target, targetEntity);
    const bool targetStructure = objectTarget && hasKindOf(
        ecs::try_get<ObjectKindOfComponent>(*m_registry, targetEntity),
        game::ObjectKindOf::Structure);
    switch (weapon->damageType) {
    case game::DamageType::SURRENDER:
        // The original ground-target branch leaves SURRENDER on VoiceAttack.
        // Its object-target branch distinguishes building clear from subdue.
        if (!objectTarget) return WorldCommandWeaponVoiceKind::None;
        return targetStructure ? WorldCommandWeaponVoiceKind::ClearBuilding
                               : WorldCommandWeaponVoiceKind::Subdue;
    case game::DamageType::DISARM:
        return WorldCommandWeaponVoiceKind::Disarm;
    case game::DamageType::KILL_PILOT:
        return commandButtonWeapon
            ? WorldCommandWeaponVoiceKind::SnipePilot
            : WorldCommandWeaponVoiceKind::None;
    case game::DamageType::MELEE:
        return commandButtonWeapon ? WorldCommandWeaponVoiceKind::Melee
                                   : WorldCommandWeaponVoiceKind::None;
    case game::DamageType::FLAME:
        if (!objectTarget && requestedWeaponSlot && *requestedWeaponSlot != 0u)
            return WorldCommandWeaponVoiceKind::FlameLocation;
        return WorldCommandWeaponVoiceKind::None;
    case game::DamageType::POISON:
        if (!objectTarget && requestedWeaponSlot && *requestedWeaponSlot != 0u)
            return WorldCommandWeaponVoiceKind::PoisonLocation;
        return WorldCommandWeaponVoiceKind::None;
    default:
        break;
    }
    // Retail contains this one content identity exception in its ground-target
    // branch. Keep it inside the data query rather than turning command input
    // or renderer state into a string protocol.
    if (!objectTarget && commandButtonWeapon &&
        container::asciiEqualIgnoreCase(weapon->name, "ComancheRocketPodWeapon")) {
        return WorldCommandWeaponVoiceKind::FireRocketPods;
    }
    return WorldCommandWeaponVoiceKind::None;
}

bool WorldCommandQueryPort::usesWorkerShoesMoveVoice(
    ObjectId actor) const noexcept {
    ecs::entity entity = ecs::null;
    if (!m_players || !usableEntity(*m_registry, *m_objects, actor, entity)) {
        return false;
    }
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(*m_registry, entity);
    // This intentionally preserves the retail conjunction.  KINDOF_DOZER and
    // KINDOF_HARVESTER do not mean either/or here: the GLA worker authors all
    // three tags, while other infantry and construction units retain VoiceMove.
    if (!hasKindOf(kinds, game::ObjectKindOf::Infantry) ||
        !hasKindOf(kinds, game::ObjectKindOf::Dozer) ||
        !hasKindOf(kinds, game::ObjectKindOf::Harvester)) {
        return false;
    }
    const std::optional<PlayerId> owner = m_ownership->ownerOf(actor);
    const UpgradeCatalog* upgrades = m_content->upgradeCatalog();
    return owner && upgrades && m_players->hasUpgradeComplete(
        *owner, well_known_upgrade::GlaWorkerShoes, *upgrades);
}

} // namespace engine::selection
