#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionCrateSalvageRandom.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/runtime/ObjectStatus.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

constexpr int32_t kRadarEventInfiltration = 6;

[[nodiscard]] bool playableMiscAudio(
    container::StringView eventName) noexcept {
    return !eventName.empty() &&
        !container::asciiEqualIgnoreCase(eventName, "NoSound");
}

[[nodiscard]] uint64_t saturatingPresentationTickAdd(
    uint64_t value, uint64_t delta) noexcept {
    return value > std::numeric_limits<uint64_t>::max() - delta
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

} // namespace

bool detail::GameSessionWeaponEventDrain::changeObjectOwner(
    ObjectId object, PlayerId owner, uint64_t confirmedTick) {
    if (!m_content.m_active || !m_content.m_players.get(owner)) return false;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    const OwnerComponent* current = entity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
        : nullptr;
    if (!current || current->player == owner) return false;
    const std::optional<ObjectTeamId> defaultTeam =
        m_world.m_objectTeams.defaultTeam(owner);
    return defaultTeam && m_ownership.transferObjectToTeam(
        object, *defaultTeam, confirmedTick);
}

void detail::GameSessionWeaponEventDrain::applyCratePickupCommands(
    container::Vector<ObjectCratePickupCommand> crateCommands) {
    // CrateCollide runs after the final movement/physics writer but emits
    // only detached commands. Apply each cross-domain effect here, where the
    // authoritative Player, shroud and central spawn services already meet.
    bool queuedCrateBodyDamage = false;
    const auto positionAvailable = [this](math::q32_32 x,
                                           math::q32_32 y,
                                           math::q32_32 radius) {
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    const TransformComponent>(m_world.m_registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            if (!identity.id || m_world.m_objects.isPendingDestroy(identity.id)) continue;
            const TransformComponent& transform =
                view.template get<const TransformComponent>(entity);
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
            const math::q32_32 otherRadius =
                geometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          geometry->boundingCircleRadiusFixed)
                    : math::q32_32{int32_t{1}};
            const LogicFixedVec3 otherPosition =
                readAuthoritativeObjectPosition(
                    m_world.m_registry,
                    entity, transform);
            const math::q32_32 dx =
                otherPosition.x - x;
            const math::q32_32 dy =
                otherPosition.y - y;
            const math::q32_32 combined = radius + otherRadius;
            if (dx * dx + dy * dy < combined * combined) return false;
        }
        return true;
    };
    container::Vector<ObjectId> consumedCrateSources;
    for (ObjectCratePickupCommand& command : crateCommands) {
        const bool sourceAlreadyConsumed = std::binary_search(
            consumedCrateSources.begin(), consumedCrateSources.end(),
            command.crate);
        if (sourceAlreadyConsumed && !command.allowMultiPickup) continue;
        switch (command.kind) {
        case game::ObjectCrateCollideKind::Money:
            if (m_content.m_players.get(command.player)) {
                if (command.moneyAmount > 0) {
                    if (m_content.m_players.adjustCash(
                            command.player, command.moneyAmount)) {
                        static_cast<void>(m_content.m_players.recordMoneyEarned(
                            command.player,
                            static_cast<uint64_t>(command.moneyAmount),
                            command.confirmedTick));
                    }
                }
                command.effectApplied = true;
            }
            break;
        case game::ObjectCrateCollideKind::Heal:
            if (m_content.m_players.get(command.player)) {
                uint32_t sequence = 0;
                for (const ObjectId object : m_world.m_ownership.objects(command.player)) {
                    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
                    ObjectHealthComponent* health = entity
                        ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity) : nullptr;
                    if (!health || health->effectivelyDead ||
                        health->currentFixed >= health->maximumFixed) continue;
                    m_world.m_objectSimulation.queueDamage({
                        .target = object,
                        .source = command.picker,
                        .sourceSequence = sequence++,
                        .amount = health->maximumFixed - health->currentFixed,
                        .damageType = game::DamageType::HEALING,
                        .deathType = game::DeathType::NORMAL,
                        .confirmedTick = m_presentation.m_confirmedTick,
                    });
                    queuedCrateBodyDamage = true;
                }
                command.effectApplied = true;
            }
            break;
        case game::ObjectCrateCollideKind::Shroud:
            if (m_content.m_players.get(command.player)) {
                static_cast<void>(m_world.m_mapVisibility.revealAll(command.player));
                command.effectApplied = true;
            }
            break;
        case game::ObjectCrateCollideKind::Unit: {
            const container::SharedPtr<const game::ObjectArchetype> unit =
                m_content.m_contentSnapshot.findObjectArchetype(command.unitName);
            if (!unit || !m_content.m_players.get(command.player)) break;
            const math::q32_32 radius = math::q32_32::max(
                math::q32_32::from_fraction(1, 2),
                unit->templateData.geometry.boundingCircleRadiusFixed);
            constexpr math::q32_32 kGoldenAngle =
                math::q32_32::from_raw(10'307'763'583ll);
            const math::q32_32 originX = command.position.x;
            const math::q32_32 originY = command.position.y;
            for (uint32_t unitIndex = 0; unitIndex < command.unitCount; ++unitIndex) {
                LogicFixedVec3 spawnPosition = command.position;
                bool foundPosition = false;
                for (uint32_t attempt = 0; attempt < 32; ++attempt) {
                    const math::q32_32 distance = math::q32_32::min(
                        math::q32_32{int32_t{20}},
                        radius + math::q32_32{int32_t{2}} +
                            math::q32_32{static_cast<int32_t>(attempt / 8u)} *
                                math::q32_32{int32_t{5}});
                    const uint64_t ordinal64 =
                        static_cast<uint64_t>(unitIndex) * 32u +
                        attempt + 1u;
                    const int32_t ordinal = static_cast<int32_t>(std::min<
                        uint64_t>(ordinal64,
                            static_cast<uint64_t>(
                                std::numeric_limits<int32_t>::max())));
                    const math::q32_32_sincos direction =
                        math::fixed_sincos(
                            kGoldenAngle * math::q32_32{ordinal});
                    const math::q32_32 x =
                        originX + direction.cosine * distance;
                    const math::q32_32 y =
                        originY + direction.sine * distance;
                    if (!positionAvailable(x, y, radius)) continue;
                    spawnPosition.x = x;
                    spawnPosition.y = y;
                    spawnPosition.z = math::q32_32::from_raw(
                        m_content.m_terrain.groundHeightRaw(
                            x.raw(), y.raw()));
                    foundPosition = true;
                    break;
                }
                if (!foundPosition) {
                    spawnPosition.z = math::q32_32::from_raw(
                        m_content.m_terrain.groundHeightRaw(
                            spawnPosition.x.raw(), spawnPosition.y.raw()));
                }
                static_cast<void>(m_lifecycle.spawnObject({
                    .templateName = command.unitName,
                    .owner = command.player,
                    .transform = ObjectFixedTransformComponent{
                        .position = spawnPosition,
                        .yawRadians = command.rotationRadians,
                        .authoritative = true,
                    },
                    .origin = ObjectCreationOrigin::System,
                    .confirmedTick = m_presentation.m_confirmedTick,
                }));
            }
            // RefCode consumes the crate once UnitName resolves, even if an
            // individual allocation/placement fails or UnitCount is zero.
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::Veterancy: {
            const PlayerState* player = m_content.m_players.get(command.player);
            if (!player || command.veterancyLevelsToGain == 0) break;

            const std::optional<ecs::entity> pickerEntity =
                m_world.m_objects.entityFromId(command.picker);
            const ObjectMapStatusComponent* pickerMap = pickerEntity
                ? ecs::try_get<ObjectMapStatusComponent>(
                      m_world.m_registry, *pickerEntity)
                : nullptr;
            const bool pickerOffMap = pickerMap && pickerMap->offMap;
            container::Vector<ObjectId> targets;
            if (command.veterancyEffectRange == 0) {
                targets.push_back(command.picker);
            } else {
                const math::q32_32 range{
                    static_cast<int32_t>(std::min<uint32_t>(
                        command.veterancyEffectRange,
                        static_cast<uint32_t>(
                            std::numeric_limits<int32_t>::max())))};
                const math::q32_32 rangeSquared = range * range;
                for (const ObjectId object : m_world.m_ownership.objects(command.player)) {
                    const std::optional<ecs::entity> entity =
                        m_world.m_objects.entityFromId(object);
                    if (!entity) continue;
                    const TransformComponent* transform =
                        ecs::try_get<TransformComponent>(
                            m_world.m_registry, *entity);
                    if (!transform) continue;
                    const ObjectMapStatusComponent* map =
                        ecs::try_get<ObjectMapStatusComponent>(
                            m_world.m_registry, *entity);
                    if ((map && map->offMap) != pickerOffMap) continue;
                    const LogicFixedVec3 targetPosition =
                        readAuthoritativeObjectPosition(
                            m_world.m_registry,
                            *entity, *transform);
                    const math::q32_32 dx =
                        targetPosition.x - command.position.x;
                    const math::q32_32 dy =
                        targetPosition.y - command.position.y;
                    if (dx * dx + dy * dy <= rangeSquared) {
                        targets.push_back(object);
                    }
                }
            }
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()),
                          targets.end());

            bool appliedAny = false;
            for (const ObjectId target : targets) {
                const std::optional<ecs::entity> targetEntity =
                    m_world.m_objects.entityFromId(target);
                if (!targetEntity) continue;
                const ObjectExperienceComponent* experience =
                    ecs::try_get<ObjectExperienceComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectVeterancyComponent* veterancy =
                    ecs::try_get<ObjectVeterancyComponent>(
                        m_world.m_registry, *targetEntity);
                const ThingTemplateComponent* type =
                    ecs::try_get<ThingTemplateComponent>(
                        m_world.m_registry, *targetEntity);
                if (!experience || !experience->trainable || !veterancy ||
                    !type || !type->archetype) {
                    continue;
                }
                const uint32_t current =
                    static_cast<uint32_t>(veterancy->level);
                const uint32_t heroic =
                    static_cast<uint32_t>(
                        game::ObjectVeterancyLevel::Heroic);
                if (current >= heroic ||
                    command.veterancyLevelsToGain > heroic - current) {
                    continue;
                }
                const size_t targetLevel =
                    static_cast<size_t>(
                        current + command.veterancyLevelsToGain);
                const int64_t required =
                    type->archetype->templateData
                        .experienceRequired[targetLevel];
                const int64_t needed = std::clamp<int64_t>(
                    required - experience->currentPoints,
                    std::numeric_limits<int32_t>::min(),
                    std::numeric_limits<int32_t>::max());
                if (needed <= 0) continue;
                const ObjectExperienceMutation mutation =
                    m_world.m_objectSimulation.addObjectExperience(
                        m_world.m_registry, m_world.m_objects, target,
                        static_cast<int32_t>(needed), false, command.crate,
                        player->upgrades.completed,
                        command.confirmedTick,
                        {.players = &m_content.m_players,
                         .scienceCatalog =
                             m_content.m_contentSnapshot.scienceCatalog(),
                         .content = &m_content.m_contentSnapshot,
                         .random = &m_content.m_simulationRandom,
                         .terrain = &m_content.m_terrain,
                         .effects = &m_world.m_objectSimulation});
                appliedAny = appliedAny || mutation.accepted;
            }
            if (appliedAny && command.veterancyIsPilot) {
                static_cast<void>(m_presentation.m_scriptObjects.transferObjectNames(
                    command.crate, command.picker));
            }
            command.effectApplied = appliedAny;
            break;
        }
        case game::ObjectCrateCollideKind::ConvertToCarBomb: {
            std::optional<ecs::entity> targetEntity =
                m_world.m_objects.entityFromId(command.picker);
            std::optional<ecs::entity> sourceEntity =
                m_world.m_objects.entityFromIdIncludingPending(command.crate);
            const PlayerState* sourcePlayer = m_content.m_players.get(command.player);
            if (!targetEntity || !sourceEntity || !sourcePlayer) break;

            // checkAndDetonateBoobyTrap runs before conversion in RefCode.
            // Resolve every attached booby-trap bomb in stable ObjectId order
            // and commit its Body effects before deciding whether either
            // participant survived the contact transaction.
            container::Vector<ObjectId> boobyTraps;
            const auto bombView = ecs::view<const ObjectIdentityComponent,
                                            ObjectStickyBombComponent>(
                m_world.m_registry);
            for (const ecs::entity bombEntity : bombView) {
                const ObjectIdentityComponent& identity =
                    bombView.template get<const ObjectIdentityComponent>(
                        bombEntity);
                const ObjectStickyBombComponent& bomb =
                    bombView.template get<ObjectStickyBombComponent>(
                        bombEntity);
                if (!identity.id || !bomb.boobyTrap) continue;
                const bool attachedToTarget = std::any_of(
                    bomb.instances.begin(), bomb.instances.end(),
                    [&command](const ObjectStickyBombRuntime& runtime) {
                        return runtime.attached && !runtime.detonated &&
                            runtime.target == command.picker;
                    });
                if (attachedToTarget) boobyTraps.push_back(identity.id);
            }
            std::sort(boobyTraps.begin(), boobyTraps.end());
            bool detonatedBoobyTrap = false;
            for (const ObjectId bomb : boobyTraps) {
                detonatedBoobyTrap =
                    m_world.m_objectSimulation.detonateStickyBomb(
                        m_world.m_registry, m_world.m_objects, m_content.m_contentSnapshot, bomb,
                        sticky_bomb::DetonationTrigger::BoobyTrap,
                        command.confirmedTick) || detonatedBoobyTrap;
            }
            if (detonatedBoobyTrap) {
                m_lifecycle.resolveQueuedObjectDamage();
                targetEntity = m_world.m_objects.entityFromId(command.picker);
                sourceEntity = m_world.m_objects.entityFromIdIncludingPending(
                    command.crate);
                const ObjectHealthComponent* targetHealth = targetEntity
                    ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry,
                                                           *targetEntity)
                    : nullptr;
                const ObjectHealthComponent* sourceHealth = sourceEntity
                    ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry,
                                                           *sourceEntity)
                    : nullptr;
                if (!targetEntity || !sourceEntity ||
                    (targetHealth && targetHealth->effectivelyDead) ||
                    (sourceHealth && sourceHealth->effectivelyDead)) {
                    break;
                }
            }
            ObjectCombatProfileComponent* combat =
                ecs::try_get<ObjectCombatProfileComponent>(
                    m_world.m_registry, *targetEntity);
            if (!combat || !combat->profile) break;

            const game::WeaponSetConditionMask carBomb =
                game::weaponSetConditionBit(
                    game::WeaponSetCondition::CarBomb);
            const game::WeaponSetProfile* selected =
                combat->profile->findBestWeaponSet(
                    combat->weaponConditions | carBomb);
            if (!selected || (selected->conditions & carBomb) == 0 ||
                (combat->weaponConditions & carBomb) != 0) {
                break;
            }

            combat->weaponConditions |= carBomb;
            static_cast<void>(refreshObjectWeaponSet(
                m_world.m_registry, *targetEntity,
                m_content.m_contentSnapshot,
                m_world.m_objectSimulation.rules()
                    .logicFramesPerSecond,
                command.confirmedTick));
            static_cast<void>(ObjectStatusSystem::apply(
                m_world.m_registry, *targetEntity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::IsCarBomb),
                 .confirmedTick = command.confirmedTick}));
            static_cast<void>(changeObjectOwner(
                command.picker, command.player, command.confirmedTick));

            const ObjectVeterancyComponent* sourceVeterancy =
                ecs::try_get<ObjectVeterancyComponent>(m_world.m_registry,
                                                        *sourceEntity);
            if (sourceVeterancy) {
                static_cast<void>(
                    m_world.m_objectSimulation.setObjectVeterancyLevel(
                        m_world.m_registry, m_world.m_objects, command.picker,
                        sourceVeterancy->level,
                        sourcePlayer->upgrades.completed,
                        command.confirmedTick,
                        {.players = &m_content.m_players,
                         .scienceCatalog =
                             m_content.m_contentSnapshot.scienceCatalog(),
                         .content = &m_content.m_contentSnapshot,
                         .random = &m_content.m_simulationRandom,
                         .terrain = &m_content.m_terrain,
                         .effects = &m_world.m_objectSimulation}));
            }

            // The terrorist's authored map/script identity follows the car,
            // matching RefCode's transferObjectName without exposing the
            // ScriptRuntime to this gameplay transaction.
            static_cast<void>(m_presentation.m_scriptObjects.transferObjectNames(
                command.crate, command.picker));
            copyObjectVisionRanges(
                m_world.m_registry, *sourceEntity, *targetEntity);
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::ConvertToHijackedVehicle: {
            const std::optional<ecs::entity> targetEntity =
                m_world.m_objects.entityFromId(command.picker);
            const std::optional<ecs::entity> sourceEntity =
                m_world.m_objects.entityFromIdIncludingPending(command.crate);
            const PlayerState* sourcePlayer = m_content.m_players.get(command.player);
            if (!targetEntity || !sourceEntity || !sourcePlayer) break;

            // RefCode plays the "vehicle stolen" warning before the defection,
            // for whoever is currently viewing the vehicle - that is, the
            // player about to lose it.
            if (const OwnerComponent* hijackVictim =
                    ecs::try_get<OwnerComponent>(
                        m_world.m_registry, *targetEntity)) {
                const PlayerState* localPlayer =
                    m_content.m_players.localPlayer();
                if (localPlayer && localPlayer->id == hijackVictim->player &&
                    hijackVictim->player != command.player) {
                    GameSessionEvaEventPublisher{m_content, m_publication}
                        .publish(
                            audio::EvaEventType::VehicleStolen,
                            command.confirmedTick,
                            (static_cast<uint64_t>(command.picker.value)
                             << 32u) ^
                                static_cast<uint64_t>(command.crate.value));
                }
            }

            const bool ownerChanged = changeObjectOwner(
                command.picker, command.player, command.confirmedTick);
            static_cast<void>(ObjectStatusSystem::apply(
                m_world.m_registry, *targetEntity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::Hijacked),
                 .confirmedTick = command.confirmedTick}));

            const ObjectVeterancyComponent* targetVeterancy =
                ecs::try_get<ObjectVeterancyComponent>(
                    m_world.m_registry, *targetEntity);
            const ObjectVeterancyComponent* sourceVeterancy =
                ecs::try_get<ObjectVeterancyComponent>(
                    m_world.m_registry, *sourceEntity);
            if (targetVeterancy && sourceVeterancy) {
                const game::ObjectVeterancyLevel highest =
                    std::max(targetVeterancy->level,
                             sourceVeterancy->level);
                if (sourceVeterancy->level < highest) {
                    static_cast<void>(
                        m_world.m_objectSimulation.setObjectVeterancyLevel(
                            m_world.m_registry, m_world.m_objects, command.crate, highest,
                            sourcePlayer->upgrades.completed,
                            command.confirmedTick,
                            {.players = &m_content.m_players,
                             .scienceCatalog =
                                 m_content.m_contentSnapshot.scienceCatalog(),
                             .content = &m_content.m_contentSnapshot,
                             .random = &m_content.m_simulationRandom,
                             .terrain = &m_content.m_terrain,
                             .effects = &m_world.m_objectSimulation}));
                }
                if (targetVeterancy->level < highest) {
                    static_cast<void>(
                        m_world.m_objectSimulation.setObjectVeterancyLevel(
                            m_world.m_registry, m_world.m_objects, command.picker, highest,
                            sourcePlayer->upgrades.completed,
                            command.confirmedTick,
                            {.players = &m_content.m_players,
                             .scienceCatalog =
                                 m_content.m_contentSnapshot.scienceCatalog(),
                             .content = &m_content.m_contentSnapshot,
                             .random = &m_content.m_simulationRandom,
                             .terrain = &m_content.m_terrain,
                             .effects = &m_world.m_objectSimulation}));
                }
            }

            if (ownerChanged) {
                // A captured Dozer must not finish construction/repair work
                // commissioned by its previous owner. Builder owns the
                // claims, leases and scaffold teardown, so cancel through its
                // typed boundary before clearing the generic order queue.
                static_cast<void>(
                    m_world.m_objectSimulation.cancelAllObjectBuilderTasks(
                        m_world.m_registry, m_world.m_objects, command.picker,
                        command.confirmedTick));
                static_cast<void>(m_presentation.m_scriptObjects.transferObjectNames(
                    command.crate, command.picker));
                copyObjectVisionRanges(
                    m_world.m_registry, *sourceEntity, *targetEntity);
                ObjectOrderQueueComponent* targetQueue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        m_world.m_registry, *targetEntity);
                if (targetQueue && !targetQueue->orders.empty()) {
                    targetQueue->orders.clear();
                    ++targetQueue->revision;
                    ++targetQueue->externalRevision;
                    if (targetQueue->externalRevision == 0)
                        ++targetQueue->externalRevision;
                }
                command.pickupAudio = "HijackDriver";
                ObjectOrderQueueComponent* sourceQueue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        m_world.m_registry, *sourceEntity);
                if (sourceQueue) {
                    sourceQueue->orders.clear();
                    ++sourceQueue->revision;
                    ++sourceQueue->externalRevision;
                    if (sourceQueue->externalRevision == 0)
                        ++sourceQueue->externalRevision;
                }
                if (!m_world.m_objects.isPendingDestroy(command.crate) &&
                    !m_world.m_objectSimulation.requestTransportBehavior(
                        m_world.m_registry, m_world.m_objects, {
                            .kind = ObjectTransportBehaviorRequestKind::HijackTarget,
                            .object = command.crate,
                            .target = command.picker,
                            .confirmedTick = command.confirmedTick,
                        })) {
                    static_cast<void>(m_lifecycle.requestDestroyObject(
                        command.crate, ObjectDestroyReason::System,
                        command.confirmedTick));
                }
            }
            command.effectApplied = ownerChanged;
            break;
        }
        case game::ObjectCrateCollideKind::Salvage: {
            const std::optional<ecs::entity> pickerEntity =
                m_world.m_objects.entityFromId(command.picker);
            const PlayerState* player = m_content.m_players.get(command.player);
            if (!pickerEntity || !player) break;
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(
                    m_world.m_registry, *pickerEntity);
            ObjectCombatProfileComponent* combat =
                ecs::try_get<ObjectCombatProfileComponent>(
                    m_world.m_registry, *pickerEntity);
            constexpr uint64_t kWeaponChancePurpose =
                0x53414c5657504e43ull; // "SALVWPNC"
            constexpr uint64_t kLevelChancePurpose =
                0x53414c564c564c43ull; // "SALVLVLC"
            constexpr uint64_t kMoneyPurpose =
                0x53414c564d4f4e59ull; // "SALVMONY"

            const game::ArmorSetConditionMask armorOne =
                game::armorSetConditionBit(
                    game::ArmorSetCondition::CrateUpgradeOne);
            const game::ArmorSetConditionMask armorTwo =
                game::armorSetConditionBit(
                    game::ArmorSetCondition::CrateUpgradeTwo);
            const game::WeaponSetConditionMask weaponOne =
                game::weaponSetConditionBit(
                    game::WeaponSetCondition::CrateUpgradeOne);
            const game::WeaponSetConditionMask weaponTwo =
                game::weaponSetConditionBit(
                    game::WeaponSetCondition::CrateUpgradeTwo);
            const bool armorEligible = combat &&
                kinds && game::objectHasKind(
                    kinds->mask, game::ObjectKindOf::ArmorSalvager) &&
                (combat->armorConditions & armorTwo) == 0;
            const bool weaponEligible = combat &&
                kinds && game::objectHasKind(
                    kinds->mask, game::ObjectKindOf::WeaponSalvager) &&
                (combat->weaponConditions & weaponTwo) == 0;

            if (armorEligible) {
                if ((combat->armorConditions & armorOne) != 0) {
                    combat->armorConditions =
                        (combat->armorConditions & ~armorOne) | armorTwo;
                } else {
                    combat->armorConditions |= armorOne;
                }
                if (ObjectArmorComponent* armor =
                        ecs::try_get<ObjectArmorComponent>(
                            m_world.m_registry, *pickerEntity)) {
                    refreshResolvedObjectArmor(*combat, *armor);
                }
                if (RenderModelComponent* visual =
                        ecs::try_get<RenderModelComponent>(
                            m_world.m_registry, *pickerEntity)) {
                    static const game::ModelConditionMask allSalvageArmor =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::ArmorsetCrateUpgradeOne,
                            game::ModelConditionFlag::ArmorsetCrateUpgradeTwo);
                    static const game::ModelConditionMask salvageArmorOne =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::ArmorsetCrateUpgradeOne);
                    static const game::ModelConditionMask salvageArmorTwo =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::ArmorsetCrateUpgradeTwo);
                    visual->modelConditionFlags.clear(allSalvageArmor);
                    const game::ModelConditionMask& selected =
                        (combat->armorConditions & armorTwo) != 0
                            ? salvageArmorTwo : salvageArmorOne;
                    visual->modelConditionFlags.words[0] |= selected.words[0];
                    visual->modelConditionFlags.words[1] |= selected.words[1];
                }
                command.salvageReward =
                    game::ObjectSalvageCrateReward::Armor;
                command.pickupAudio = "CrateSalvage";
            } else if (weaponEligible && crate_salvage::chanceSucceeds(
                           static_cast<uint64_t>(m_content.m_startInfo.seed), command,
                           command.salvageWeaponChance,
                           kWeaponChancePurpose)) {
                if ((combat->weaponConditions & weaponOne) != 0) {
                    combat->weaponConditions =
                        (combat->weaponConditions & ~weaponOne) | weaponTwo;
                } else {
                    combat->weaponConditions |= weaponOne;
                }
                static_cast<void>(refreshObjectWeaponSet(
                    m_world.m_registry, *pickerEntity,
                    m_content.m_contentSnapshot,
                    m_world.m_objectSimulation.rules()
                        .logicFramesPerSecond,
                    command.confirmedTick));
                // RefCode Object::setWeaponSetFlag/clearWeaponSetFlag
                // (Object.cpp:3135/3146) project every weapon-set flag onto its
                // model condition through TheWeaponSetTypeToModelConditionTypeMap
                // (WeaponSet.h:86-106). The armor branch above already mirrors
                // its pair; this is the missing weapon half.
                //
                // Deliberately narrow rather than a generic projection inside
                // refreshObjectWeaponSet: WEAPONSET_PLAYER_UPGRADE and RIDER1-8
                // model conditions are legitimately set by name elsewhere
                // (ObjectUpgradeEffects string masks, ObjectContainmentTransfer),
                // and modelConditionFlags here is a direct write with no
                // per-source ownership, so a generic clear+set keyed on
                // combat->weaponConditions would erase those.
                if (RenderModelComponent* visual =
                        ecs::try_get<RenderModelComponent>(
                            m_world.m_registry, *pickerEntity)) {
                    static const game::ModelConditionMask allSalvageWeapon =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::WeaponsetCrateUpgradeOne,
                            game::ModelConditionFlag::WeaponsetCrateUpgradeTwo);
                    static const game::ModelConditionMask salvageWeaponOne =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::WeaponsetCrateUpgradeOne);
                    static const game::ModelConditionMask salvageWeaponTwo =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::WeaponsetCrateUpgradeTwo);
                    visual->modelConditionFlags.clear(allSalvageWeapon);
                    const game::ModelConditionMask& selected =
                        (combat->weaponConditions & weaponTwo) != 0
                            ? salvageWeaponTwo : salvageWeaponOne;
                    visual->modelConditionFlags.words[0] |= selected.words[0];
                    visual->modelConditionFlags.words[1] |= selected.words[1];
                }
                command.salvageReward =
                    game::ObjectSalvageCrateReward::Weapon;
                command.pickupAudio = "CrateSalvage";
            } else {
                const ObjectExperienceComponent* experience =
                    ecs::try_get<ObjectExperienceComponent>(
                        m_world.m_registry, *pickerEntity);
                const ObjectVeterancyComponent* veterancy =
                    ecs::try_get<ObjectVeterancyComponent>(
                        m_world.m_registry, *pickerEntity);
                const ThingTemplateComponent* type =
                    ecs::try_get<ThingTemplateComponent>(
                        m_world.m_registry, *pickerEntity);
                const bool levelEligible = experience && experience->trainable &&
                    veterancy && type && type->archetype &&
                    veterancy->level != game::ObjectVeterancyLevel::Heroic;
                if (levelEligible && crate_salvage::chanceSucceeds(
                        static_cast<uint64_t>(m_content.m_startInfo.seed), command,
                        command.salvageLevelChance,
                        kLevelChancePurpose)) {
                    const size_t nextLevel =
                        static_cast<size_t>(veterancy->level) + 1u;
                    const int64_t required =
                        type->archetype->templateData
                            .experienceRequired[nextLevel];
                    const int64_t needed = std::clamp<int64_t>(
                        required - experience->currentPoints,
                        std::numeric_limits<int32_t>::min(),
                        std::numeric_limits<int32_t>::max());
                    static_cast<void>(m_world.m_objectSimulation.addObjectExperience(
                        m_world.m_registry, m_world.m_objects, command.picker,
                        static_cast<int32_t>(needed), true, command.crate,
                        player->upgrades.completed,
                        command.confirmedTick,
                        {.players = &m_content.m_players,
                         .scienceCatalog =
                             m_content.m_contentSnapshot.scienceCatalog(),
                         .content = &m_content.m_contentSnapshot,
                         .random = &m_content.m_simulationRandom,
                         .terrain = &m_content.m_terrain,
                         .effects = &m_world.m_objectSimulation}));
                    command.salvageReward =
                        game::ObjectSalvageCrateReward::Veterancy;
                } else {
                    command.moneyAmount = crate_salvage::randomInteger(
                        static_cast<uint64_t>(m_content.m_startInfo.seed), command,
                        command.salvageMinimumMoney,
                        command.salvageMaximumMoney, kMoneyPurpose);
                    if (command.moneyAmount > 0) {
                        if (m_content.m_players.adjustCash(
                                command.player, command.moneyAmount)) {
                            static_cast<void>(m_content.m_players.recordMoneyEarned(
                                command.player,
                                static_cast<uint64_t>(command.moneyAmount),
                                command.confirmedTick));
                        }
                    }
                    command.salvageReward =
                        game::ObjectSalvageCrateReward::Money;
                    command.pickupAudio = "CrateMoney";
                }
            }
            static_cast<void>(m_content.m_players.recordAcademyEvent(
                command.player,
                PlayerAcademyEvent::SalvageCollected));
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::SabotageFakeBuilding: {
            const std::optional<ecs::entity> target =
                m_world.m_objects.entityFromId(command.picker);
            const ObjectHealthComponent* health = target
                ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *target)
                : nullptr;
            if (!health || health->effectivelyDead) break;
            m_world.m_objectSimulation.queueDamage({
                .target = command.picker,
                .source = command.crate,
                .amount = health->maximumFixed,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::DETONATED,
                .confirmedTick = command.confirmedTick,
            });
            queuedCrateBodyDamage = true;
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::SabotageInternetCenter:
        case game::ObjectCrateCollideKind::SabotageMilitaryFactory: {
            const std::optional<ecs::entity> target =
                m_world.m_objects.entityFromId(command.picker);
            if (!target) break;
            const uint64_t fps = std::max<uint32_t>(
                1u, m_world.m_objectSimulation.rules().logicFramesPerSecond);
            const uint64_t scaled =
                static_cast<uint64_t>(command.sabotageDurationMilliseconds) * fps;
            const uint64_t durationTicks =
                scaled / 1000u + (scaled % 1000u != 0 ? 1u : 0u);
            const uint64_t deadline = durationTicks >
                    std::numeric_limits<uint64_t>::max() - command.confirmedTick
                ? std::numeric_limits<uint64_t>::max()
                : command.confirmedTick + durationTicks;
            static_cast<void>(ObjectDisabledSystem::setUntil(
                m_world.m_registry, *target, ObjectDisabledReason::Hacked,
                deadline, command.confirmedTick));
            if (command.kind ==
                game::ObjectCrateCollideKind::SabotageInternetCenter) {
                static_cast<void>(
                    m_world.m_objectSimulation.setPlayerSpyVisionDisabledUntil(
                        m_world.m_registry, m_world.m_objects, command.victimPlayer,
                        deadline, command.confirmedTick));
                const ObjectContainmentComponent* contained =
                    ecs::try_get<ObjectContainmentComponent>(m_world.m_registry,
                                                              *target);
                if (contained) {
                    for (const ObjectContainedObjectRecord& record :
                         contained->objects) {
                        const std::optional<ecs::entity> occupant =
                            m_world.m_objects.entityFromId(record.object);
                        if (!occupant ||
                            m_world.m_objects.isPendingDestroy(record.object)) {
                            continue;
                        }
                        static_cast<void>(ObjectDisabledSystem::setUntil(
                            m_world.m_registry, *occupant,
                            ObjectDisabledReason::Hacked, deadline,
                            command.confirmedTick));
                    }
                }
            }
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::SabotagePowerPlant: {
            if (!m_content.m_players.get(command.victimPlayer)) break;
            const uint64_t fps = std::max<uint32_t>(
                1u, m_world.m_objectSimulation.rules().logicFramesPerSecond);
            const uint64_t scaled =
                static_cast<uint64_t>(command.sabotageDurationMilliseconds) * fps;
            const uint64_t durationTicks =
                scaled / 1000u + (scaled % 1000u != 0 ? 1u : 0u);
            const uint64_t deadline = durationTicks >
                    std::numeric_limits<uint64_t>::max() - command.confirmedTick
                ? std::numeric_limits<uint64_t>::max()
                : command.confirmedTick + durationTicks;
            static_cast<void>(m_content.m_players.setPowerSabotagedUntil(
                command.victimPlayer, deadline));
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::SabotageSupplyCenter:
        case game::ObjectCrateCollideKind::SabotageSupplyDropzone: {
            const PlayerState* victim = m_content.m_players.get(command.victimPlayer);
            const PlayerState* saboteur = m_content.m_players.get(command.player);
            if (!victim || !saboteur) break;
            if (command.kind ==
                game::ObjectCrateCollideKind::SabotageSupplyDropzone) {
                static_cast<void>(m_world.m_objectSimulation.resetObjectOclTimers(
                    m_world.m_registry, m_world.m_objects, command.picker,
                    m_content.m_simulationRandom, command.confirmedTick));
            }
            const int64_t desired = static_cast<int64_t>(
                command.stealCashAmount);
            const int64_t capacity =
                std::numeric_limits<int64_t>::max() - saboteur->cash;
            const int64_t stolen = std::min({
                victim->cash, desired, capacity});
            if (stolen > 0 &&
                m_content.m_players.trySpend(command.victimPlayer, stolen)) {
                if (m_content.m_players.adjustCash(command.player, stolen)) {
                    static_cast<void>(m_content.m_players.recordMoneyEarned(
                        command.player,
                        static_cast<uint64_t>(stolen),
                        command.confirmedTick));
                    command.moneyAmount = stolen;
                }
            }
            command.effectApplied = true;
            break;
        }
        case game::ObjectCrateCollideKind::SabotageCommandCenter:
        case game::ObjectCrateCollideKind::SabotageSuperweapon: {
            if (!m_world.m_objects.entityFromId(command.picker)) break;
            static_cast<void>(
                m_world.m_objectSimulation.restartAllSpecialPowerRecharge(
                    m_world.m_registry, m_world.m_objects, command.picker,
                    m_content.m_contentSnapshot, command.confirmedTick));
            command.effectApplied = true;
            break;
        }
        }

        const bool sabotage =
            command.kind == game::ObjectCrateCollideKind::
                SabotageCommandCenter ||
            command.kind == game::ObjectCrateCollideKind::
                SabotageFakeBuilding ||
            command.kind == game::ObjectCrateCollideKind::
                SabotageInternetCenter ||
            command.kind == game::ObjectCrateCollideKind::
                SabotageMilitaryFactory ||
            command.kind == game::ObjectCrateCollideKind::
                SabotagePowerPlant ||
            command.kind == game::ObjectCrateCollideKind::
                SabotageSuperweapon ||
            command.kind == game::ObjectCrateCollideKind::
                SabotageSupplyCenter ||
            command.kind == game::ObjectCrateCollideKind::
                SabotageSupplyDropzone;
        if (command.effectApplied && sabotage) {
            const bool supplySabotage =
                command.kind == game::ObjectCrateCollideKind::
                    SabotageSupplyCenter ||
                command.kind == game::ObjectCrateCollideKind::
                    SabotageSupplyDropzone;
            const bool cashStolen = supplySabotage &&
                command.moneyAmount > 0;
            const PlayerState* localPlayer =
                m_content.m_players.localPlayer();
            auto& presentation = m_presentation;
            const RenderObjectFeedbackGameData& feedbackSettings =
                presentation.m_renderGameDataSettings.visual.objectFeedback;
            const uint32_t logicFramesPerSecond = static_cast<uint32_t>(
                std::max(1,
                    m_content.m_startInfo.gameSpeedFPS));
            if (localPlayer && localPlayer->id == command.victimPlayer) {
                // Radar::tryInfiltrationEvent is observer-local and does not
                // depend on whether the player currently owns a radar. The
                // four-second marker, UI message and warning sound form one
                // presentation transaction for the victim.
                if (presentation.m_scriptPresentationSequence !=
                    std::numeric_limits<uint64_t>::max()) {
                    ++presentation.m_scriptPresentationSequence;
                }
                if (presentation.m_scriptPresentationSequence == 0)
                    presentation.m_scriptPresentationSequence = 1;
                const uint64_t dieTick = saturatingPresentationTickAdd(
                    command.confirmedTick,
                    static_cast<uint64_t>(logicFramesPerSecond) * 4u);
                presentation.m_scriptMapPresentation.appendRadarEvent({
                    .position = {
                        command.position.x.to_float(),
                        command.position.y.to_float(),
                        command.position.z.to_float()},
                    .eventType = kRadarEventInfiltration,
                    .stamp = {
                        .presentationEpoch =
                            presentation.m_scriptPresentationEpoch,
                        .sequence =
                            presentation.m_scriptPresentationSequence,
                        .confirmedTick = command.confirmedTick,
                        .sourceScriptId = 0,
                        .ordinal = command.authoredOrder,
                    },
                    .fadeTick = dieTick - std::min<uint64_t>(
                        dieTick, logicFramesPerSecond / 2u),
                    .dieTick = dieTick,
                });
                m_publication.emitScriptSessionEvent({
                    .kind = script::ScriptSessionEventKind::Text,
                    .confirmedTick = command.confirmedTick,
                    .ordinal = command.authoredOrder,
                    .text = "RADAR:Infiltration",
                    .localized = true,
                });
                if (playableMiscAudio(
                        feedbackSettings.radarInfiltrationAudioEvent)) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName =
                            feedbackSettings.radarInfiltrationAudioEvent,
                        .sourcePlayer = localPlayer->id,
                    }));
                }
                GameSessionEvaEventPublisher{m_content, m_publication}
                    .publish(
                    cashStolen
                        ? audio::EvaEventType::CashStolen
                        : audio::EvaEventType::BuildingSabotaged,
                    command.confirmedTick,
                    (static_cast<uint64_t>(command.crate.value) << 32u) ^
                        static_cast<uint64_t>(command.picker.value) ^
                        static_cast<uint64_t>(command.authoredOrder));
            }
            if (command.kind != game::ObjectCrateCollideKind::
                    SabotageFakeBuilding) {
                const container::String* sabotageAudio = nullptr;
                switch (command.kind) {
                case game::ObjectCrateCollideKind::SabotageCommandCenter:
                case game::ObjectCrateCollideKind::SabotageSuperweapon:
                    sabotageAudio =
                        &feedbackSettings.sabotageResetTimerAudioEvent;
                    break;
                case game::ObjectCrateCollideKind::SabotageSupplyCenter:
                case game::ObjectCrateCollideKind::SabotageSupplyDropzone:
                    sabotageAudio = &feedbackSettings.moneyWithdrawAudioEvent;
                    break;
                default:
                    sabotageAudio =
                        &feedbackSettings.sabotageShutdownAudioEvent;
                    break;
                }
                if (sabotageAudio && playableMiscAudio(*sabotageAudio)) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = *sabotageAudio,
                        .emitter = command.picker,
                        .owner = command.picker,
                        .position = math::vec3{
                            command.position.x.to_float(),
                            command.position.y.to_float(),
                            command.position.z.to_float()},
                    }));
                }

                if (presentation.m_objectFeedbackOrdinal !=
                    std::numeric_limits<uint64_t>::max()) {
                    ++presentation.m_objectFeedbackOrdinal;
                }
                if (presentation.m_objectFeedbackOrdinal == 0)
                    presentation.m_objectFeedbackOrdinal = 1;
                const uint32_t decayTicks =
                    script::ScriptObjectPresentationState::flashDecayTicks(
                        logicFramesPerSecond);
                presentation.m_objectSelectionFlashes[command.picker] = {
                    .identity = presentation.m_objectFeedbackOrdinal,
                    .startTick = command.confirmedTick,
                    .expireTick = saturatingPresentationTickAdd(
                        command.confirmedTick,
                        std::max<uint32_t>(1u, decayTicks)),
                };
            }
            if (cashStolen) {
                LogicFixedVec3 incomePosition = command.cratePosition;
                incomePosition.z += math::q32_32{int32_t{20}};
                publishObjectCashFloatingText(
                    command.crate, incomePosition, command.moneyAmount,
                    0xff00ff00u, command.confirmedTick);

                LogicFixedVec3 lossPosition = command.position;
                lossPosition.z += math::q32_32{int32_t{30}};
                publishObjectCashFloatingText(
                    command.picker, lossPosition, -command.moneyAmount,
                    0xffff0000u, command.confirmedTick);
            }
        }

        command.executeBehaviorReturnedTrue =
            command.effectApplied && !command.preserveSourceOnSuccess;
        if (command.executeBehaviorReturnedTrue &&
            !sourceAlreadyConsumed) {
            const auto insertion = std::lower_bound(
                consumedCrateSources.begin(), consumedCrateSources.end(),
                command.crate);
            consumedCrateSources.insert(insertion, command.crate);
        }

        const std::optional<game::FxInvocationAnchor> pickerAnchor =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, command.picker);
        if (command.executeBehaviorReturnedTrue &&
            !command.executeFx.empty() && pickerAnchor) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = command.executeFx,
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = *pickerAnchor,
            }));
        }
        if (command.effectApplied && !command.convertFxList.empty() &&
            pickerAnchor) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = command.convertFxList,
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = *pickerAnchor,
            }));
        }
        // CrateCollide::onCollide has committed the salvage outcome at this
        // point.  Treat that as the semantic event for VoiceSalvage rather
        // than an optimistic contextual click: many crates are collected by
        // autonomous collision, and failed eligibility/RNG branches must not
        // claim a reward audibly.  The per-unit acknowledgement is local,
        // exactly like the original command translator's response.
        if (command.kind == game::ObjectCrateCollideKind::Salvage &&
            command.effectApplied) {
            const PlayerState* localPlayer =
                m_content.m_players.localPlayer();
            const std::optional<ecs::entity> pickerEntity =
                m_world.m_objects.entityFromIdIncludingPending(command.picker);
            const ThingTemplateComponent* pickerType = pickerEntity
                ? ecs::try_get<ThingTemplateComponent>(
                      m_world.m_registry, *pickerEntity)
                : nullptr;
            if (localPlayer && localPlayer->id == command.player &&
                pickerType && pickerType->archetype) {
                const container::StringView cue =
                    pickerType->archetype->templateData.perUnitSound(
                        "VoiceSalvage");
                if (!cue.empty()) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = container::String{cue},
                        .emitter = command.picker,
                        .owner = command.picker,
                        .position = pickerAnchor
                            ? std::optional<math::vec3>{
                                  pickerAnchor->position}
                            : std::nullopt,
                    }));
                }
            }
        }
        const char* audioEvent = nullptr;
        switch (command.kind) {
        case game::ObjectCrateCollideKind::Heal: audioEvent = "CrateHeal"; break;
        case game::ObjectCrateCollideKind::Money: audioEvent = "CrateMoney"; break;
        case game::ObjectCrateCollideKind::Shroud: audioEvent = "CrateShroud"; break;
        case game::ObjectCrateCollideKind::Unit: audioEvent = "CrateFreeUnit"; break;
        case game::ObjectCrateCollideKind::Veterancy: audioEvent = nullptr; break;
        case game::ObjectCrateCollideKind::ConvertToCarBomb: audioEvent = nullptr; break;
        case game::ObjectCrateCollideKind::ConvertToHijackedVehicle:
            audioEvent = command.pickupAudio.empty()
                ? nullptr : command.pickupAudio.c_str();
            break;
        case game::ObjectCrateCollideKind::Salvage:
            audioEvent = command.pickupAudio.empty()
                ? nullptr : command.pickupAudio.c_str();
            break;
        case game::ObjectCrateCollideKind::SabotageCommandCenter:
        case game::ObjectCrateCollideKind::SabotageFakeBuilding:
        case game::ObjectCrateCollideKind::SabotageInternetCenter:
        case game::ObjectCrateCollideKind::SabotageMilitaryFactory:
        case game::ObjectCrateCollideKind::SabotagePowerPlant:
        case game::ObjectCrateCollideKind::SabotageSuperweapon:
        case game::ObjectCrateCollideKind::SabotageSupplyCenter:
        case game::ObjectCrateCollideKind::SabotageSupplyDropzone:
            audioEvent = nullptr;
            break;
        }
        if (command.effectApplied && audioEvent) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = audioEvent,
                .emitter = command.picker,
                .owner = command.picker,
                .position = math::vec3{
                    command.position.x.to_float(),
                    command.position.y.to_float(),
                    command.position.z.to_float()},
            }));
        }
    }
    // CrateCollide::onCollide destroys the source only after the concrete
    // executeCrateBehavior returns true.  Delay the lifecycle commit until
    // every detached command has observed that result; this also preserves
    // explicit AllowMultiPickup behavior without exposing a pending-destroy
    // source to later same-frame commands.
    for (const ObjectId source : consumedCrateSources) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            source, ObjectDestroyReason::System, m_presentation.m_confirmedTick));
    }
    if (queuedCrateBodyDamage) {
        m_lifecycle.resolveQueuedObjectDamage();
    }
}

} // namespace engine
