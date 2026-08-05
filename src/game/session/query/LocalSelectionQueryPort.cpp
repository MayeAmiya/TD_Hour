#include "game/session/query/LocalSelectionQueryPort.h"

#include "core/container/string_utils.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/session/ai/GameSessionAIDomain.h"

namespace engine::selection {
namespace {

[[nodiscard]] bool componentHasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

LocalSelectionObjectSnapshot LocalSelectionQueryPort::inspect(
    PlayerId localPlayer, ObjectId object) const noexcept {
    LocalSelectionObjectSnapshot facts{.object = object};
    if (!object || m_objects->isPendingDestroy(object)) return facts;
    const std::optional<ecs::entity> entity =
        m_objects->entityFromId(object);
    if (!entity) return facts;

    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(*m_registry, *entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(*m_registry, *entity);
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(*m_registry, *entity);
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(*m_registry, *entity);
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(*m_registry, *entity);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(*m_registry, *entity);
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *entity);

    const bool effectivelyDead = health && health->effectivelyDead;
    const bool alwaysSelectable = componentHasKind(
        kinds, game::ObjectKindOf::AlwaysSelectable);
    const bool blockedByStatus = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Unselectable) |
        game::objectStatusBit(game::ObjectStatusFlag::Masked));
    facts.live = (!lifecycle ||
                  lifecycle->phase == ObjectLifecyclePhase::Alive) &&
        (!map || !map->offMap) &&
        (!contained || !contained->container || !contained->enclosing) &&
        !effectivelyDead;
    facts.selectable = (!lifecycle ||
                        lifecycle->phase == ObjectLifecyclePhase::Alive) &&
        (!map || !map->offMap) &&
        (componentHasKind(kinds, game::ObjectKindOf::Selectable) ||
         alwaysSelectable) &&
        (!contained || !contained->container || !contained->enclosing) &&
        !blockedByStatus && (!effectivelyDead || alwaysSelectable);
    facts.owner = m_ownership->ownerOf(object).value_or(INVALID_PLAYER_ID);
    facts.local = localPlayer && facts.owner == localPlayer;
    facts.structure = componentHasKind(kinds, game::ObjectKindOf::Structure);
    facts.healPad = componentHasKind(kinds, game::ObjectKindOf::HealPad);
    facts.carBomb = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::IsCarBomb));
    if (type) {
        facts.type = type->name;
        if (type->archetype) {
            facts.displayNameLabel =
                type->archetype->templateData.displayName;
        }
    }
    return facts;
}

container::String LocalSelectionQueryPort::voiceCue(
    ObjectId object, LocalUnitVoiceCue cue) const {
    if (!object) return {};
    const std::optional<ecs::entity> entity =
        m_objects->entityFromId(object);
    if (!entity) return {};
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *entity);
    if (!type || !type->archetype) return {};
    const game::ThingTemplate& templateData = type->archetype->templateData;
    switch (cue) {
    case LocalUnitVoiceCue::Select:
        return templateData.voiceSelect;
    case LocalUnitVoiceCue::GroupSelect:
        // RefCode documents this as the response for selecting a group of this
        // unit; only 11 Zero Hour objects author it, so falling back to the
        // ordinary select cue is what keeps a group selection audible at all.
        return templateData.voiceGroupSelect.empty()
            ? templateData.voiceSelect : templateData.voiceGroupSelect;
    case LocalUnitVoiceCue::Move:
        return templateData.voiceMove;
    case LocalUnitVoiceCue::MoveUpgraded:
        return container::String{
            templateData.perUnitSound("VoiceMoveUpgraded")};
    case LocalUnitVoiceCue::Attack:
        return templateData.voiceAttack;
    case LocalUnitVoiceCue::AttackAir:
        // An anti-air order falls back to the ordinary attack acknowledgement
        // rather than going silent, matching CommandXlat's priority walk.
        return templateData.voiceAttackAir.empty()
            ? templateData.voiceAttack : templateData.voiceAttackAir;
    case LocalUnitVoiceCue::Guard:
        return templateData.voiceGuard;
    case LocalUnitVoiceCue::Supply:
        return container::String{templateData.perUnitSound("VoiceSupply")};
    case LocalUnitVoiceCue::Repair:
        return container::String{templateData.perUnitSound("VoiceRepair")};
    case LocalUnitVoiceCue::BuildResponse:
        return container::String{
            templateData.perUnitSound("VoiceBuildResponse")};
    case LocalUnitVoiceCue::WeaponPrimaryMode:
        return container::String{
            templateData.perUnitSound("VoicePrimaryWeaponMode")};
    case LocalUnitVoiceCue::WeaponSecondaryMode:
        return container::String{
            templateData.perUnitSound("VoiceSecondaryWeaponMode")};
    case LocalUnitVoiceCue::WeaponTertiaryMode:
        return container::String{
            templateData.perUnitSound("VoiceTertiaryWeaponMode")};
    case LocalUnitVoiceCue::Bombard:
        return container::String{templateData.perUnitSound("VoiceBombard")};
    case LocalUnitVoiceCue::CombatDrop:
        return container::String{templateData.perUnitSound("VoiceCombatDrop")};
    case LocalUnitVoiceCue::HackInternet:
        return container::String{
            templateData.perUnitSound("VoiceHackInternet")};
    case LocalUnitVoiceCue::Salvage:
        return container::String{templateData.perUnitSound("VoiceSalvage")};
    case LocalUnitVoiceCue::ClearBuilding:
        return container::String{
            templateData.perUnitSound("VoiceClearBuilding")};
    case LocalUnitVoiceCue::Subdue:
        return container::String{templateData.perUnitSound("VoiceSubdue")};
    case LocalUnitVoiceCue::Disarm:
        return container::String{templateData.perUnitSound("VoiceDisarm")};
    case LocalUnitVoiceCue::SnipePilot:
        return container::String{
            templateData.perUnitSound("VoiceSnipePilot")};
    case LocalUnitVoiceCue::Melee:
        return container::String{templateData.perUnitSound("VoiceMelee")};
    case LocalUnitVoiceCue::FlameLocation:
        return container::String{
            templateData.perUnitSound("VoiceFlameLocation")};
    case LocalUnitVoiceCue::PoisonLocation:
        return container::String{
            templateData.perUnitSound("VoicePoisonLocation")};
    case LocalUnitVoiceCue::FireRocketPods:
        return container::String{
            templateData.perUnitSound("VoiceFireRocketPods")};
    case LocalUnitVoiceCue::Enter:
        return container::String{templateData.resolvedVoiceEnter()};
    case LocalUnitVoiceCue::Garrison:
        return container::String{templateData.resolvedVoiceGarrison()};
    case LocalUnitVoiceCue::EnterHostile:
        return container::String{templateData.perUnitSound("VoiceEnterHostile")};
    case LocalUnitVoiceCue::Crush:
        return container::String{templateData.perUnitSound("VoiceCrush")};
    case LocalUnitVoiceCue::Unload:
        return container::String{templateData.perUnitSound("VoiceUnload")};
    case LocalUnitVoiceCue::GetHealed:
        return container::String{templateData.perUnitSound("VoiceGetHealed")};
    }
    return {};
}

bool LocalSelectionQueryPort::matchesType(
    ObjectId object, container::StringView sought) const noexcept {
    if (sought.empty()) return true;
    const std::optional<ecs::entity> entity =
        m_objects->entityFromId(object);
    if (!entity) return false;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *entity);
    if (!type) return false;
    const container::SharedPtr<const game::ObjectArchetype> soughtArchetype =
        m_content->findObjectArchetype(sought);
    if (type->archetype && soughtArchetype) {
        return game::legacyThingTemplatesEquivalent(
            type->archetype->templateData,
            soughtArchetype->templateData);
    }
    return container::asciiEqualIgnoreCase(type->name, sought);
}

container::Vector<ObjectId> LocalSelectionQueryPort::allObjects() const {
    container::Vector<ObjectId> result;
    const auto objects = ecs::view<const ObjectIdentityComponent>(*m_registry);
    for (const ecs::entity entity : objects) {
        result.push_back(objects
            .template get<const ObjectIdentityComponent>(entity).id);
    }
    return result;
}

bool LocalSelectionQueryPort::hasKind(
    ObjectId object, game::ObjectKindOf sought) const noexcept {
    const std::optional<ecs::entity> entity =
        m_objects->entityFromId(object);
    return entity && componentHasKind(
        ecs::try_get<ObjectKindOfComponent>(*m_registry, *entity), sought);
}

bool LocalSelectionQueryPort::canCrushTarget(
    ObjectId crusher, ObjectId target) const noexcept {
    if (!crusher || !target || crusher == target) return false;
    const std::optional<ecs::entity> crusherEntity =
        m_objects->entityFromId(crusher);
    const std::optional<ecs::entity> targetEntity =
        m_objects->entityFromId(target);
    if (!crusherEntity || !targetEntity) return false;
    // RefCode never promotes VoiceMove to VoiceCrush for an ally.  Ownership
    // is the stable local approximation here; the confirmed squish system
    // remains the authority for diplomacy and every runtime exception.
    const std::optional<PlayerId> crusherOwner = m_ownership->ownerOf(crusher);
    const std::optional<PlayerId> targetOwner = m_ownership->ownerOf(target);
    if (crusherOwner && targetOwner && *crusherOwner == *targetOwner)
        return false;
    const ThingTemplateComponent* crusherType =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *crusherEntity);
    const ThingTemplateComponent* targetType =
        ecs::try_get<ThingTemplateComponent>(*m_registry, *targetEntity);
    if (!crusherType || !targetType || !crusherType->archetype ||
        !targetType->archetype) {
        return false;
    }
    const game::ThingTemplate& crusherTemplate =
        crusherType->archetype->templateData;
    const game::ThingTemplate& targetTemplate =
        targetType->archetype->templateData;
    return crusherTemplate.crusherLevel != 0 &&
        crusherTemplate.crusherLevel > targetTemplate.crushableLevel;
}

bool LocalSelectionQueryPort::isIdleAiObject(ObjectId object) const noexcept {
    const script::ScriptSequentialAuthorityState state =
        m_ai->sequentialObjectState(object);
    return state.hasAI && state.idle;
}

std::optional<math::vec3> LocalSelectionQueryPort::cameraTarget(
    ObjectId object) const noexcept {
    const std::optional<ecs::entity> entity =
        m_objects->entityFromId(object);
    if (!entity) return std::nullopt;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(*m_registry, *entity);
    if (!transform) return std::nullopt;
    const LogicFixedVec3 fixed = readAuthoritativeObjectPosition(
        *m_registry, *entity, *transform);
    return math::vec3{
        fixed.x.to_float(), fixed.y.to_float(), fixed.z.to_float()};
}

} // namespace engine::selection
