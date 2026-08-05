#include "core/container/container_types.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ObjectArchetype.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/simulation/combat/ObjectHistoricWeaponLedger.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include "game/object/simulation/combat/ObjectCombatDetail.h"

namespace engine {

using namespace object_combat_detail;

bool initializeObjectSystemWeaponRuntime(
    ObjectSystemWeaponRuntime& runtime, container::StringView weaponName,
    ecs::registry& registry, ecs::entity sourceEntity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick,
    ObjectSystemWeaponInitialLoad initialLoad) noexcept {
    runtime = {};
    if (sourceEntity == ecs::null || !registry.valid(sourceEntity)) return false;
    runtime.content = content.findWeaponId(weaponName);
    const game::WeaponTemplate* definition = content.findWeapon(runtime.content);
    if (!definition) return false;

    // FireWeaponWhenDamaged owns real private Weapon instances in RefCode.
    // Their constructors call reloadAmmo rather than loadAmmoNow: the clip is
    // filled immediately, but the weapon remains unavailable for one authored
    // ClipReloadTime measured from object creation.  Snapshot the source bonus
    // here so later activation does not accidentally restart that initial
    // reload with a newer rate-of-fire multiplier.
    runtime.ammoInClip = definition->clipSize > 0
        ? static_cast<uint32_t>(definition->clipSize)
        : std::numeric_limits<uint32_t>::max();
    runtime.scatterTargetsUnused.reserve(definition->scatterTargets.size());
    for (size_t index = 0; index < definition->scatterTargets.size(); ++index)
        runtime.scatterTargetsUnused.push_back(static_cast<uint32_t>(index));
    const ObjectWeaponBonusComponent* bonusState =
        ecs::try_get<ObjectWeaponBonusComponent>(registry, sourceEntity);
    const game::WeaponBonus bonus = content.resolveWeaponBonus(
        *definition, bonusState ? bonusState->conditions
                                : game::WeaponBonusConditionMask{});
    const uint64_t reloadFrames =
        initialLoad == ObjectSystemWeaponInitialLoad::Immediate
            ? 0 : clipReloadFrames(
                      *definition, bonus, logicFramesPerSecond);
    runtime.reloadCompleteTick = saturatingTickAdd(confirmedTick, reloadFrames);
    runtime.nextReadyTick = runtime.reloadCompleteTick;
    runtime.suspendFxUntilTick = saturatingTickAdd(
        confirmedTick,
        millisecondsToFrames(
            definition->suspendFxDelayMilliseconds,
            logicFramesPerSecond));
    return true;
}

namespace {

void copySystemWeaponProjectileSamples(
    ObjectSystemWeaponFireCommand& command,
    const game::WeaponTemplate& definition,
    const GameContentSnapshot& content,
    const ecs::registry& registry,
    ecs::entity targetEntity,
    SimulationRandom& random,
    container::Vector<uint32_t>* scatterTargetsUnused = nullptr) {
    if (definition.projectileObject.empty()) return;
    ObjectProjectileSpawnRequest request{
        .launcher = command.source,
        .intendedTarget = command.target,
        .detonationWeapon = command.content,
        .projectileTemplate = definition.projectileObject,
        .launchPosition = command.sourcePosition,
        .targetPosition = command.impactPosition,
        .sourceShotSequence = command.sourceShotSequence,
        .confirmedTick = command.confirmedTick,
    };
    const ObjectKindOfComponent* targetKinds =
        targetEntity != ecs::null && registry.valid(targetEntity)
        ? ecs::try_get<ObjectKindOfComponent>(registry, targetEntity)
        : nullptr;
    const ObjectTerrainLayerComponent* targetLayer =
        targetEntity != ecs::null && registry.valid(targetEntity)
        ? ecs::try_get<ObjectTerrainLayerComponent>(registry, targetEntity)
        : nullptr;
    applyProjectileScatter(
        request, definition, targetKinds,
        targetLayer ? targetLayer->pathfindLayer
                    : game::terrain::kGroundPathfindLayer,
        random, scatterTargetsUnused);
    populateTumbleLaunchRates(request, content, random);
    command.target = request.intendedTarget;
    command.impactPosition = request.targetPosition;
    command.targetWasScattered = request.targetWasScattered;
    command.scatteredTargetPathfindLayer =
        request.scatteredTargetPathfindLayer;
    command.hasTumbleAngularRates = request.hasTumbleAngularRates;
    command.tumbleYawRate = request.tumbleYawRate;
    command.tumblePitchRate = request.tumblePitchRate;
    command.tumbleRollRate = request.tumbleRollRate;
}

[[nodiscard]] std::optional<ObjectSystemWeaponFireCommand> makeSystemWeaponCommand(
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    ecs::entity targetEntity, ObjectId target,
    game::WeaponContentId contentId, uint32_t sourceShotSequence,
    uint32_t authoredOrder, uint64_t emissionSequence,
    uint64_t confirmedTick) {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    if (!transform) return std::nullopt;
    const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
        registry, sourceEntity, *transform);
    LogicFixedVec3 impactPosition = sourcePosition;
    if (target && targetEntity != ecs::null && registry.valid(targetEntity)) {
        if (const TransformComponent* targetTransform =
                ecs::try_get<TransformComponent>(registry, targetEntity)) {
            impactPosition = readAuthoritativeObjectPosition(
                registry, targetEntity, *targetTransform);
        }
    }
    ObjectSystemWeaponFireCommand command{
        .source = source,
        .target = target,
        .content = contentId,
        .sourcePosition = sourcePosition,
        .impactPosition = impactPosition,
        .sourceShotSequence = sourceShotSequence,
        .authoredOrder = authoredOrder,
        .emissionSequence = emissionSequence,
        .confirmedTick = confirmedTick,
    };
    if (const ObjectWeaponBonusComponent* bonus =
            ecs::try_get<ObjectWeaponBonusComponent>(registry, sourceEntity)) {
        command.bonusConditions = bonus->conditions;
    }
    return command;
}

} // namespace

namespace object_combat_detail {
namespace {

void publishContinuousFireStage(
    ObjectWeaponComponent& weapons, ObjectContinuousFireStage stage) {
    weapons.continuousFireStage = stage;
}

[[nodiscard]] bool hasWeaponBonus(
    const ecs::registry& registry, ecs::entity entity,
    game::WeaponBonusCondition condition) noexcept {
    const ObjectWeaponBonusComponent* bonus =
        ecs::try_get<ObjectWeaponBonusComponent>(registry, entity);
    return bonus &&
        (bonus->conditions & game::weaponBonusConditionBit(condition)) != 0;
}

[[nodiscard]] container::String rapidFireVoice(
    const ecs::registry& registry, ecs::entity entity) {
    const ThingTemplateComponent* thing =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!thing || !thing->archetype) return {};
    for (const auto& [semantic, eventName] :
         thing->archetype->templateData.unitSpecificSounds) {
        if (asciiEqualIgnoreCase(semantic, "VoiceRapidFire"))
            return eventName;
    }
    return {};
}

void appendTrackerAudioEvent(
    container::Vector<ObjectWeaponEvent>& events,
    ObjectWeaponEventKind kind, ObjectId object,
    game::WeaponContentId weapon, container::String eventName,
    uint64_t confirmedTick) {
    if (eventName.empty()) return;
    events.push_back({
        .kind = kind,
        .source = object,
        .content = weapon,
        .audioEventName = std::move(eventName),
        .confirmedTick = confirmedTick,
    });
}

void speedUpContinuousFire(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    ObjectWeaponComponent& weapons, const GameContentSnapshot& content,
    SimulationRandom& random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, container::Vector<ObjectWeaponEvent>& events) {
    const bool fast = hasWeaponBonus(
        registry, entity, game::WeaponBonusCondition::ContinuousFireFast);
    if (fast) return;
    const bool mean = hasWeaponBonus(
        registry, entity, game::WeaponBonusCondition::ContinuousFireMean);
    if (mean) {
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity,
            game::WeaponBonusCondition::ContinuousFireFast, true,
            &content, &random, logicFramesPerSecond, confirmedTick));
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, entity,
            game::WeaponBonusCondition::ContinuousFireMean, false,
            &content, &random, logicFramesPerSecond, confirmedTick));
        publishContinuousFireStage(
            weapons, ObjectContinuousFireStage::Fast);
        appendTrackerAudioEvent(
            events, ObjectWeaponEventKind::RapidFireVoice, object, {},
            rapidFireVoice(registry, entity), confirmedTick);
        return;
    }
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity,
        game::WeaponBonusCondition::ContinuousFireMean, true,
        &content, &random, logicFramesPerSecond, confirmedTick));
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity,
        game::WeaponBonusCondition::ContinuousFireFast, false,
        &content, &random, logicFramesPerSecond, confirmedTick));
    publishContinuousFireStage(
        weapons, ObjectContinuousFireStage::Mean);
}

void coolDownContinuousFire(
    ecs::registry& registry, ecs::entity entity,
    ObjectWeaponComponent& weapons, const GameContentSnapshot& content,
    SimulationRandom& random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) {
    const bool wasFast = hasWeaponBonus(
        registry, entity, game::WeaponBonusCondition::ContinuousFireFast);
    const bool wasMean = hasWeaponBonus(
        registry, entity, game::WeaponBonusCondition::ContinuousFireMean);
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity,
        game::WeaponBonusCondition::ContinuousFireFast, false,
        &content, &random, logicFramesPerSecond, confirmedTick));
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity,
        game::WeaponBonusCondition::ContinuousFireMean, false,
        &content, &random, logicFramesPerSecond, confirmedTick));
    if (wasFast || wasMean) {
        publishContinuousFireStage(
            weapons, ObjectContinuousFireStage::Slow);
    } else {
        publishContinuousFireStage(
            weapons, ObjectContinuousFireStage::None);
        weapons.continuousFireCooldownTick = 0;
    }
    weapons.consecutiveShots = 0;
    weapons.consecutiveShotVictim = INVALID_OBJECT_ID;
}

} // namespace

void updateObjectFiringTracker(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    ObjectWeaponComponent& weapons, const GameContentSnapshot& content,
    SimulationRandom& random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, container::Vector<ObjectWeaponEvent>& events) {
    if (weapons.forceReloadTick != 0 &&
        confirmedTick >= weapons.forceReloadTick) {
        static_cast<void>(reloadAllObjectWeaponsNow(
            registry, entity, content, confirmedTick,
            logicFramesPerSecond));
        weapons.forceReloadTick = 0;
    }
    if (weapons.loopingFireSoundStopTick != 0 &&
        confirmedTick >= weapons.loopingFireSoundStopTick) {
        const game::WeaponTemplate* loopWeapon =
            content.findWeapon(weapons.loopingFireSoundWeapon);
        appendTrackerAudioEvent(
            events, ObjectWeaponEventKind::FireSoundLoopStopped, object,
            weapons.loopingFireSoundWeapon,
            loopWeapon ? loopWeapon->fireSound : container::String{},
            confirmedTick);
        weapons.loopingFireSoundStopTick = 0;
        weapons.loopingFireSoundWeapon = {};
    }
    if (weapons.continuousFireCooldownTick != 0 &&
        confirmedTick > weapons.continuousFireCooldownTick) {
        weapons.continuousFireCooldownTick = saturatingTickAdd(
            confirmedTick, std::max<uint32_t>(1, logicFramesPerSecond));
        coolDownContinuousFire(
            registry, entity, weapons, content, random,
            logicFramesPerSecond, confirmedTick);
    }
}

void notifyObjectFiringTrackerShot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object, ObjectId victim,
    ObjectWeaponComponent& weapons, const game::WeaponTemplate& weapon,
    game::WeaponContentId weaponContent, game::WeaponSlot weaponSlot,
    uint64_t possibleNextShotTick,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectWeaponEvent>& events) {
    bool targetHasFaerieFire = false;
    if (victim) {
        const std::optional<ecs::entity> targetEntity =
            lifecycle.entityFromIdIncludingPending(victim);
        const ObjectStatusComponent* status = targetEntity
            ? ecs::try_get<ObjectStatusComponent>(registry, *targetEntity)
            : nullptr;
        targetHasFaerieFire = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::FaerieFire));
    }
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity, game::WeaponBonusCondition::TargetFaerieFire,
        targetHasFaerieFire, &content, &random, logicFramesPerSecond,
        confirmedTick));
    if (weapons.activeWeaponSetIndex &&
        *weapons.activeWeaponSetIndex < weapons.sets.size()) {
        const size_t slotIndex = static_cast<size_t>(weaponSlot);
        const ObjectWeaponSetRuntime& active =
            weapons.sets[*weapons.activeWeaponSetIndex];
        if (slotIndex < active.slots.size() &&
            active.slots[slotIndex].content == weaponContent) {
            possibleNextShotTick = active.slots[slotIndex].nextReadyTick;
        }
    }

    if (victim == weapons.consecutiveShotVictim ||
        (weapons.continuousFireCooldownTick != 0 &&
         confirmedTick < weapons.continuousFireCooldownTick)) {
        if (weapons.consecutiveShots != std::numeric_limits<uint32_t>::max())
            ++weapons.consecutiveShots;
        weapons.consecutiveShotVictim = victim;
    } else {
        weapons.consecutiveShots = 1;
        weapons.consecutiveShotVictim = victim;
    }

    const uint64_t autoReloadDelay = millisecondsToFrames(
        weapon.autoReloadWhenIdleMilliseconds, logicFramesPerSecond);
    if (autoReloadDelay != 0) {
        weapons.forceReloadTick = saturatingTickAdd(
            confirmedTick, autoReloadDelay);
    }
    const uint64_t coast = millisecondsToFrames(
        weapon.continuousFireCoastMilliseconds, logicFramesPerSecond);
    weapons.continuousFireCooldownTick = coast != 0
        ? saturatingTickAdd(possibleNextShotTick, coast) : 0;

    const bool mean = hasWeaponBonus(
        registry, entity, game::WeaponBonusCondition::ContinuousFireMean);
    const bool fast = hasWeaponBonus(
        registry, entity, game::WeaponBonusCondition::ContinuousFireFast);
    if (mean) {
        if (weapons.consecutiveShots < weapon.continuousFireOneShotsNeeded) {
            coolDownContinuousFire(
                registry, entity, weapons, content, random,
                logicFramesPerSecond, confirmedTick);
        } else if (weapons.consecutiveShots >
                   weapon.continuousFireTwoShotsNeeded) {
            speedUpContinuousFire(
                registry, entity, object, weapons, content, random,
                logicFramesPerSecond, confirmedTick, events);
        }
    } else if (fast) {
        if (weapons.consecutiveShots < weapon.continuousFireTwoShotsNeeded) {
            coolDownContinuousFire(
                registry, entity, weapons, content, random,
                logicFramesPerSecond, confirmedTick);
        }
    } else if (weapons.consecutiveShots >
               weapon.continuousFireOneShotsNeeded) {
        speedUpContinuousFire(
            registry, entity, object, weapons, content, random,
            logicFramesPerSecond, confirmedTick, events);
    }

    const uint64_t loopDuration = millisecondsToFrames(
        weapon.fireSoundLoopTimeMilliseconds, logicFramesPerSecond);
    if (loopDuration != 0) {
        if (weapons.loopingFireSoundStopTick == 0) {
            weapons.loopingFireSoundWeapon = weaponContent;
            appendTrackerAudioEvent(
                events, ObjectWeaponEventKind::FireSoundLoopStarted,
                object, weaponContent, weapon.fireSound, confirmedTick);
        }
        weapons.loopingFireSoundStopTick = saturatingTickAdd(
            confirmedTick, loopDuration);
    }
}

} // namespace object_combat_detail

bool tryQueueObjectSystemWeaponFire(
    ObjectSystemWeaponRuntime& runtime,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands,
    bool selectAsCurrentWeapon) {
    if (!source || sourceEntity == ecs::null || !registry.valid(sourceEntity)) {
        return false;
    }
    if (!ecs::try_get<TransformComponent>(registry, sourceEntity)) return false;
    const game::WeaponTemplate* definition = content.findWeapon(runtime.content);
    if (!definition) return false;

    if (runtime.reloadCompleteTick != 0 &&
        confirmedTick >= runtime.reloadCompleteTick) {
        runtime.reloadCompleteTick = 0;
        runtime.ammoInClip = definition->clipSize > 0
            ? static_cast<uint32_t>(definition->clipSize)
            : std::numeric_limits<uint32_t>::max();
        runtime.scatterTargetsUnused.clear();
        runtime.scatterTargetsUnused.reserve(definition->scatterTargets.size());
        for (size_t index = 0; index < definition->scatterTargets.size(); ++index)
            runtime.scatterTargetsUnused.push_back(static_cast<uint32_t>(index));
    }
    if (confirmedTick < runtime.nextReadyTick ||
        (runtime.reloadCompleteTick != 0 &&
         confirmedTick < runtime.reloadCompleteTick) ||
        (definition->clipSize > 0 && runtime.ammoInClip == 0)) {
        return false;
    }

    const ObjectWeaponBonusComponent* bonusState =
        ecs::try_get<ObjectWeaponBonusComponent>(registry, sourceEntity);
    const game::WeaponBonusConditionMask conditions =
        bonusState ? bonusState->conditions : game::WeaponBonusConditionMask{};
    const game::WeaponBonus bonus = content.resolveWeaponBonus(
        *definition, conditions);
    ObjectWeaponComponent* presentationWeapons = selectAsCurrentWeapon
        ? ecs::try_get<ObjectWeaponComponent>(registry, sourceEntity)
        : nullptr;
    std::optional<game::WeaponSlot> presentationSlot;
    if (presentationWeapons && presentationWeapons->activeWeaponSetIndex &&
        *presentationWeapons->activeWeaponSetIndex <
            presentationWeapons->sets.size()) {
        const ObjectWeaponSetRuntime& active = presentationWeapons->sets[
            *presentationWeapons->activeWeaponSetIndex];
        for (size_t index = 0; index < active.slots.size(); ++index) {
            if (active.slots[index].content == runtime.content) {
                presentationSlot = static_cast<game::WeaponSlot>(index);
                break;
            }
        }
    }
    const game::WeaponSlot launchSlot = presentationSlot.value_or(
        game::WeaponSlot::Primary);
    const auto firingConditions = firingPresentationConditions(
        registry, sourceEntity, launchSlot);
    const uint32_t shotSequence = runtime.nextShotSequence++;
    if (runtime.nextShotSequence == 0) ++runtime.nextShotSequence;
    const auto currentPresentation = pristineWeaponPresentation(
        registry, sourceEntity, content, launchSlot,
        firingConditions ? &*firingConditions : nullptr);
    const uint32_t barrelSequenceOrdinal =
        game::selectAndAdvanceWeaponBarrel(
            runtime.currentBarrel,
            runtime.shotsRemainingForCurrentBarrel,
            currentPresentation
                ? static_cast<uint32_t>(
                      currentPresentation->barrels.barrels.size())
                : 0u,
            definition->shotsPerBarrel);
    runtime.nextReadyTick = saturatingTickAdd(
        confirmedTick,
        chooseShotDelayFrames(*definition, logicFramesPerSecond, random, bonus));
    if (definition->clipSize > 0) {
        --runtime.ammoInClip;
        if (runtime.ammoInClip == 0 &&
            definition->reloadType == game::WeaponReloadType::Auto) {
            runtime.reloadCompleteTick = saturatingTickAdd(
                confirmedTick, std::max<uint64_t>(
                    1, clipReloadFrames(*definition, bonus,
                                        logicFramesPerSecond)));
            runtime.nextReadyTick = std::max(runtime.nextReadyTick,
                                             runtime.reloadCompleteTick);
        }
    }

    std::optional<ObjectSystemWeaponFireCommand> command = makeSystemWeaponCommand(
        registry, sourceEntity, source, ecs::null, INVALID_OBJECT_ID,
        runtime.content, shotSequence,
        authoredOrder, emissionSequence, confirmedTick);
    if (!command) return false;
    command->launchSlot = launchSlot;
    command->usesFiringPresentation = presentationSlot.has_value();
    command->sourceBarrelSequenceOrdinal = barrelSequenceOrdinal;
    command->weaponFxSuspendedByDelay =
        confirmedTick < runtime.suspendFxUntilTick;
    copySystemWeaponProjectileSamples(
        *command, *definition, content, registry, ecs::null, random,
        &runtime.scatterTargetsUnused);
    if (presentationWeapons && presentationSlot &&
        presentationWeapons->activeWeaponSetIndex &&
        *presentationWeapons->activeWeaponSetIndex <
            presentationWeapons->sets.size()) {
        ObjectWeaponSlotRuntime& mirrored = presentationWeapons->sets[
            *presentationWeapons->activeWeaponSetIndex]
            .slots[static_cast<size_t>(*presentationSlot)];
        mirrored.previousFireTick = mirrored.lastFireTick;
        mirrored.previousFireSequence = mirrored.lastFireSequence;
        mirrored.lastFireTick = confirmedTick;
        mirrored.lastFireSequence = shotSequence;
        presentationWeapons->currentSlot = *presentationSlot;
        notifyTurretWeaponFired(
            *presentationWeapons, *presentationSlot, confirmedTick);
        markObjectDirty(
            registry, sourceEntity,
            objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    }
    outCommands.push_back(std::move(*command));
    return true;
}

bool queueObjectTransientWeaponFire(
    game::WeaponContentId contentId,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t sourceShotSequence, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) {
    if (!source || sourceEntity == ecs::null || !registry.valid(sourceEntity) ||
        !contentId) return false;
    const game::WeaponTemplate* definition = content.findWeapon(contentId);
    if (!definition) return false;
    std::optional<ObjectSystemWeaponFireCommand> command = makeSystemWeaponCommand(
        registry, sourceEntity, source, ecs::null, INVALID_OBJECT_ID,
        contentId, sourceShotSequence,
        authoredOrder, emissionSequence, confirmedTick);
    if (!command) return false;
    command->sourceBarrelSequenceOrdinal =
        game::weaponBarrelSequenceOrdinal(
            sourceShotSequence, definition->shotsPerBarrel);
    command->weaponFxSuspendedByDelay =
        definition->suspendFxDelayMilliseconds != 0;
    copySystemWeaponProjectileSamples(
        *command, *definition, content, registry, ecs::null, random);
    outCommands.push_back(std::move(*command));
    return true;
}

bool queueObjectTransientWeaponFireAtPosition(
    game::WeaponContentId contentId,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    const LogicFixedVec3& impactPosition,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t sourceShotSequence, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) {
    if (!source || sourceEntity == ecs::null || !registry.valid(sourceEntity) ||
        !contentId) return false;
    const game::WeaponTemplate* definition = content.findWeapon(contentId);
    if (!definition) return false;
    std::optional<ObjectSystemWeaponFireCommand> command =
        makeSystemWeaponCommand(
            registry, sourceEntity, source, ecs::null, INVALID_OBJECT_ID,
            contentId, sourceShotSequence, authoredOrder, emissionSequence,
            confirmedTick);
    if (!command) return false;
    command->impactPosition = impactPosition;
    command->sourceBarrelSequenceOrdinal =
        game::weaponBarrelSequenceOrdinal(
            sourceShotSequence, definition->shotsPerBarrel);
    command->weaponFxSuspendedByDelay =
        definition->suspendFxDelayMilliseconds != 0;
    copySystemWeaponProjectileSamples(
        *command, *definition, content, registry, ecs::null, random);
    outCommands.push_back(std::move(*command));
    return true;
}

bool tryQueueObjectSlotWeaponFireAtPosition(
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    game::WeaponSlot selectedSlot, const LogicFixedVec3& impactPosition,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands,
    bool selectAsCurrentWeapon) {
    if (!source || sourceEntity == ecs::null ||
        !registry.valid(sourceEntity)) return false;
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, sourceEntity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) return false;
    const size_t slotIndex = static_cast<size_t>(selectedSlot);
    if (slotIndex >= game::kWeaponSlotCount) return false;

    ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    advanceWeaponSet(set, content, confirmedTick);
    ObjectWeaponSlotRuntime& runtime = set.slots[slotIndex];
    const game::WeaponTemplate* definition = content.findWeapon(runtime.content);
    if (!definition || confirmedTick < runtime.nextReadyTick ||
        isReloading(runtime, confirmedTick) ||
        (set.sharedReloadCompleteTick != 0 &&
         confirmedTick < set.sharedReloadCompleteTick) ||
        hasFiniteEmptyClip(runtime, *definition)) return false;

    const ObjectWeaponBonusComponent* bonusState =
        ecs::try_get<ObjectWeaponBonusComponent>(registry, sourceEntity);
    const game::WeaponBonusConditionMask conditions = bonusState
        ? bonusState->conditions : game::WeaponBonusConditionMask{};
    const game::WeaponBonus bonus =
        content.resolveWeaponBonus(*definition, conditions);
    const uint32_t shotSequence = weapons->nextShotSequence++;
    if (weapons->nextShotSequence == 0) ++weapons->nextShotSequence;
    const auto firingConditions = firingPresentationConditions(
        registry, sourceEntity, selectedSlot);
    const auto presentation = pristineWeaponPresentation(
        registry, sourceEntity, content, selectedSlot,
        firingConditions ? &*firingConditions : nullptr);
    const uint32_t barrelSequenceOrdinal =
        game::selectAndAdvanceWeaponBarrel(
            runtime.currentBarrel, runtime.shotsRemainingForCurrentBarrel,
            presentation
                ? static_cast<uint32_t>(presentation->barrels.barrels.size())
                : 0u,
            definition->shotsPerBarrel);

    const uint64_t readyAt = saturatingTickAdd(
        confirmedTick,
        chooseShotDelayFrames(*definition, logicFramesPerSecond, random,
                              bonus));
    runtime.nextReadyTick = readyAt;
    runtime.previousFireTick = runtime.lastFireTick;
    runtime.previousFireSequence = runtime.lastFireSequence;
    runtime.lastFireTick = confirmedTick;
    runtime.lastFireSequence = shotSequence;
    notifyTurretWeaponFired(*weapons, selectedSlot, confirmedTick);
    if (definition->clipSize > 0) {
        if (runtime.ammoInClip > 0) --runtime.ammoInClip;
        if (runtime.ammoInClip == 0 &&
            definition->reloadType == game::WeaponReloadType::Auto) {
            runtime.reloadCompleteTick = saturatingTickAdd(
                confirmedTick,
                std::max<uint64_t>(
                    1u, clipReloadFrames(*definition, bonus,
                                        logicFramesPerSecond)));
            runtime.reloadReplenishesClip = true;
            runtime.nextReadyTick = std::max(
                runtime.nextReadyTick, runtime.reloadCompleteTick);
            if (set.shareWeaponReloadTime)
                set.sharedReloadCompleteTick = std::max(
                    set.sharedReloadCompleteTick,
                    runtime.reloadCompleteTick);
        }
    }
    if (set.shareWeaponReloadTime) {
        const uint64_t sharedReadyAt = std::max(
            readyAt, set.sharedReloadCompleteTick);
        for (ObjectWeaponSlotRuntime& sibling : set.slots)
            sibling.nextReadyTick = std::max(
                sibling.nextReadyTick, sharedReadyAt);
    }
    if (selectAsCurrentWeapon)
        weapons->currentSlot = selectedSlot;

    std::optional<ObjectSystemWeaponFireCommand> command =
        makeSystemWeaponCommand(
            registry, sourceEntity, source, ecs::null, INVALID_OBJECT_ID,
            runtime.content, shotSequence, authoredOrder, emissionSequence,
            confirmedTick);
    if (!command) return false;
    command->impactPosition = impactPosition;
    command->launchSlot = selectedSlot;
    command->usesFiringPresentation = true;
    command->sourceBarrelSequenceOrdinal = barrelSequenceOrdinal;
    command->weaponFxSuspendedByDelay =
        confirmedTick < runtime.suspendFxUntilTick;
    copySystemWeaponProjectileSamples(
        *command, *definition, content, registry, ecs::null, random,
        &runtime.scatterTargetsUnused);
    // This helper is used outside ObjectCombatFrame by script waypoint fire,
    // transport payload/strafe transactions and Spectre. Those callers do
    // not own the frame-level weapon fingerprint guard, so the confirmed
    // lastFire pulse would otherwise remain invisible to
    // ObjectModelConditionAuthority and FIRING_A/B/C would never reach the
    // Draw channel. Publish the same sparse edge only after the detached
    // weapon command itself has been admitted.
    markObjectDirty(
        registry, sourceEntity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    outCommands.push_back(std::move(*command));
    return true;
}

bool queueObjectTargetedTransientWeaponFire(
    game::WeaponContentId contentId,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    ecs::entity targetEntity, ObjectId target,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t sourceShotSequence, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) {
    if (!source || !target || source == target ||
        sourceEntity == ecs::null || targetEntity == ecs::null ||
        !registry.valid(sourceEntity) || !registry.valid(targetEntity) ||
        !contentId) return false;
    const game::WeaponTemplate* definition = content.findWeapon(contentId);
    if (!definition) return false;
    std::optional<ObjectSystemWeaponFireCommand> command =
        makeSystemWeaponCommand(
            registry, sourceEntity, source, targetEntity, target, contentId,
            sourceShotSequence, authoredOrder, emissionSequence,
            confirmedTick);
    if (!command) return false;
    command->sourceBarrelSequenceOrdinal =
        game::weaponBarrelSequenceOrdinal(
            sourceShotSequence, definition->shotsPerBarrel);
    command->weaponFxSuspendedByDelay =
        definition->suspendFxDelayMilliseconds != 0;
    copySystemWeaponProjectileSamples(
        *command, *definition, content, registry, targetEntity, random);
    outCommands.push_back(std::move(*command));
    return true;
}

bool setObjectWeaponLock(ecs::registry& registry, ecs::entity entity,
                         game::WeaponSlot slot,
                         ObjectWeaponLockType type) noexcept {
    if (entity == ecs::null || !registry.valid(entity) ||
        type == ObjectWeaponLockType::None) {
        return false;
    }
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= game::kWeaponSlotCount ||
        !weapons->sets[*weapons->activeWeaponSetIndex].slots[slotIndex].content) {
        return false;
    }

    // RefCode reports success for a valid temporary request even when an
    // existing permanent lock prevents that request from taking ownership.
    if (type == ObjectWeaponLockType::Temporary &&
        weapons->lockType == ObjectWeaponLockType::Permanent) {
        return true;
    }
    weapons->lockedSlot = slot;
    weapons->currentSlot = slot;
    weapons->lockType = type;
    return true;
}

bool releaseObjectWeaponLock(ecs::registry& registry, ecs::entity entity,
                             ObjectWeaponLockType type) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return false;
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    return weapons && releaseWeaponLock(*weapons, type);
}

bool refreshObjectWeaponSet(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return false;
    const ObjectCombatProfileComponent* combat =
        ecs::try_get<ObjectCombatProfileComponent>(registry, entity);
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!combat || !combat->profile || !weapons) return false;

    const container::Span<const game::WeaponSetProfile> authoredSets =
        combat->profile->weaponSets();
    const game::WeaponSetProfile* selected =
        combat->profile->findBestWeaponSet(combat->weaponConditions);
    if (!selected || authoredSets.empty()) return false;
    const size_t setIndex = static_cast<size_t>(selected - authoredSets.data());
    return activateWeaponSetRuntime(
        *weapons, setIndex, content, logicFramesPerSecond, confirmedTick);
}

bool reloadAllObjectWeaponsNow(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return false;
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons) return false;
    static_cast<void>(refreshObjectWeaponSet(
        registry, entity, content, logicFramesPerSecond, confirmedTick));
    if (!weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }

    ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    bool reloaded = false;
    for (ObjectWeaponSlotRuntime& slot : set.slots) {
        const game::WeaponTemplate* definition =
            content.findWeapon(slot.content);
        if (!definition) continue;
        if (!set.shareWeaponReloadTime && definition->clipSize > 0 &&
            slot.ammoInClip == static_cast<uint32_t>(definition->clipSize)) {
            continue;
        }
        if (definition->clipSize > 0) {
            slot.ammoInClip = static_cast<uint32_t>(definition->clipSize);
            rebuildScatterTargets(slot, *definition);
            ++slot.clipGeneration;
            if (slot.clipGeneration == 0) slot.clipGeneration = 1;
        }
        slot.nextReadyTick = confirmedTick;
        slot.reloadCompleteTick = 0;
        slot.reloadReplenishesClip = false;
        reloaded = true;
    }
    if (set.shareWeaponReloadTime && reloaded) {
        set.sharedReloadCompleteTick = 0;
        for (ObjectWeaponSlotRuntime& slot : set.slots) {
            if (slot.content) slot.nextReadyTick = confirmedTick;
        }
    }
    return true;
}

bool objectIsOutOfReturnToBaseAmmo(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return false;
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }

    const ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    uint32_t specialWeapons = 0;
    uint32_t emptySpecialWeapons = 0;
    for (const ObjectWeaponSlotRuntime& slot : set.slots) {
        const game::WeaponTemplate* definition =
            content.findWeapon(slot.content);
        if (!definition || definition->reloadType !=
                game::WeaponReloadType::ReturnToBase) {
            continue;
        }
        ++specialWeapons;
        if (definition->clipSize > 0 && slot.ammoInClip == 0)
            ++emptySpecialWeapons;
    }
    return specialWeapons != 0 && emptySpecialWeapons == specialWeapons;
}

bool objectIsOutOfAmmo(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return true;
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return true;
    }

    const ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    for (const ObjectWeaponSlotRuntime& slot : set.slots) {
        const game::WeaponTemplate* definition =
            content.findWeapon(slot.content);
        if (!definition) continue;

        // Weapon::getStatus never resolves an auto-reloading empty clip to
        // terminal OUT_OF_AMMO: privateFireWeapon immediately enters reload,
        // and the weapon becomes READY_TO_FIRE when that reload completes.
        if (definition->clipSize <= 0 || slot.ammoInClip != 0 ||
            definition->reloadType == game::WeaponReloadType::Auto) {
            return false;
        }
    }
    return true;
}

uint64_t objectAirfieldReloadDurationFrames(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content,
    uint32_t logicFramesPerSecond) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return 1;
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return 1;
    }
    const ObjectWeaponBonusComponent* bonusState =
        ecs::try_get<ObjectWeaponBonusComponent>(registry, entity);
    const game::WeaponBonusConditionMask conditions = bonusState
        ? bonusState->conditions : game::WeaponBonusConditionMask{};
    const ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    uint64_t longest = 0;
    for (const ObjectWeaponSlotRuntime& slot : set.slots) {
        const game::WeaponTemplate* definition =
            content.findWeapon(slot.content);
        if (!definition || definition->clipSize <= 0) continue;
        const uint32_t clipSize = static_cast<uint32_t>(definition->clipSize);
        const uint32_t remaining = std::min(slot.ammoInClip, clipSize);
        const uint32_t needed = clipSize - remaining;
        const game::WeaponBonus bonus =
            content.resolveWeaponBonus(*definition, conditions);
        const uint64_t full = clipReloadFrames(
            *definition, bonus, logicFramesPerSecond);
        const uint64_t proportional = clipSize != 0
            ? (full * static_cast<uint64_t>(needed)) / clipSize : 0;
        longest = std::max(longest, proportional);
    }
    return std::max<uint64_t>(1, longest);
}

bool applyObjectAirfieldReloadProgress(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint64_t elapsedFrames,
    uint64_t totalFrames, uint64_t confirmedTick) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return false;
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }
    totalFrames = std::max<uint64_t>(1, totalFrames);
    elapsedFrames = std::min(elapsedFrames, totalFrames);
    ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    bool changed = false;
    for (ObjectWeaponSlotRuntime& slot : set.slots) {
        const game::WeaponTemplate* definition =
            content.findWeapon(slot.content);
        if (!definition || definition->clipSize <= 0) continue;
        const uint32_t clipSize = static_cast<uint32_t>(definition->clipSize);
        const uint64_t scaled = static_cast<uint64_t>(clipSize) * elapsedFrames;
        const uint32_t desired = elapsedFrames >= totalFrames
            ? clipSize : static_cast<uint32_t>(scaled / totalFrames);
        if (slot.ammoInClip != desired) {
            slot.ammoInClip = desired;
            ++slot.clipGeneration;
            if (slot.clipGeneration == 0) slot.clipGeneration = 1;
            changed = true;
        }
        if (elapsedFrames >= totalFrames) {
            rebuildScatterTargets(slot, *definition);
            slot.nextReadyTick = confirmedTick;
            slot.reloadCompleteTick = 0;
            slot.reloadReplenishesClip = false;
        } else {
            const uint64_t remaining = totalFrames - elapsedFrames;
            slot.nextReadyTick = saturatingTickAdd(confirmedTick, remaining);
            slot.reloadCompleteTick = slot.nextReadyTick;
            slot.reloadReplenishesClip = false;
        }
    }
    set.sharedReloadCompleteTick = elapsedFrames >= totalFrames
        ? 0 : saturatingTickAdd(confirmedTick, totalFrames - elapsedFrames);
    return changed || elapsedFrames >= totalFrames;
}

bool setObjectWeaponBonusCondition(
    ecs::registry& registry, ecs::entity entity,
    game::WeaponBonusCondition condition, bool enabled,
    const GameContentSnapshot* content, SimulationRandom* random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick) {
    const game::WeaponBonusConditionMask bit = game::weaponBonusConditionBit(condition);
    if (entity == ecs::null || bit == 0 || !registry.valid(entity)) return false;

    ObjectWeaponBonusComponent* state =
        ecs::try_get<ObjectWeaponBonusComponent>(registry, entity);
    if (!state) {
        if (!enabled) return false;
        state = &ecs::emplace<ObjectWeaponBonusComponent>(registry, entity);
    }
    const game::WeaponBonusConditionMask next = enabled
        ? static_cast<game::WeaponBonusConditionMask>(state->conditions | bit)
        : static_cast<game::WeaponBonusConditionMask>(state->conditions & ~bit);
    if (next == state->conditions) return false;

    state->conditions = next;
    if (state->revision != std::numeric_limits<uint64_t>::max()) ++state->revision;
    state->lastChangedTick = confirmedTick;
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));

    ObjectWeaponComponent* weapons = ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!content || !random || !weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return true;
    }

    ObjectWeaponSetRuntime& set = weapons->sets[*weapons->activeWeaponSetIndex];
    std::optional<size_t> sharedInitiator;
    if (set.shareWeaponReloadTime) {
        for (size_t index = 0; index < set.slots.size(); ++index) {
            const ObjectWeaponSlotRuntime& candidate = set.slots[index];
            if (!candidate.content ||
                (!isReloading(candidate, confirmedTick) &&
                 candidate.nextReadyTick <= confirmedTick)) {
                continue;
            }
            if (!sharedInitiator) {
                sharedInitiator = index;
                continue;
            }
            const ObjectWeaponSlotRuntime& selected = set.slots[*sharedInitiator];
            // The slot that exhausted its clip is the real shared-reload
            // initiator. Otherwise use the most recently fired slot. This
            // deliberately fixes the RefCode loop bug that repeatedly turned
            // every sibling into a reload initiator and let the final slot's
            // unrelated template duration win.
            if ((!selected.reloadReplenishesClip && candidate.reloadReplenishesClip) ||
                (selected.reloadReplenishesClip == candidate.reloadReplenishesClip &&
                 candidate.lastFireTick > selected.lastFireTick)) {
                sharedInitiator = index;
            }
        }
    }
    uint64_t sharedDeadline = 0;
    bool refreshSharedGate = false;
    bool refreshSharedReloadGate =
        set.sharedReloadCompleteTick != 0 && confirmedTick < set.sharedReloadCompleteTick;
    for (size_t index = 0; index < set.slots.size(); ++index) {
        if (sharedInitiator && index != *sharedInitiator) continue;
        ObjectWeaponSlotRuntime& slot = set.slots[index];
        const game::WeaponTemplate* definition = content->findWeapon(slot.content);
        if (!definition) continue;
        const bool reloading = isReloading(slot, confirmedTick);
        const bool betweenShots = !reloading && slot.nextReadyTick > confirmedTick;
        if (!reloading && !betweenShots) continue;

        const game::WeaponBonus bonus = content->resolveWeaponBonus(*definition, next);
        const uint64_t delay = reloading
            ? clipReloadFrames(*definition, bonus, logicFramesPerSecond)
            : chooseShotDelayFrames(*definition, logicFramesPerSecond, *random, bonus);
        const uint64_t deadline = saturatingTickAdd(confirmedTick, delay);
        slot.nextReadyTick = deadline;
        if (reloading) slot.reloadCompleteTick = deadline;
        if (set.shareWeaponReloadTime) {
            sharedDeadline = std::max(sharedDeadline, deadline);
            refreshSharedGate = true;
            refreshSharedReloadGate = refreshSharedReloadGate || reloading;
        }
    }

    if (refreshSharedGate) {
        if (refreshSharedReloadGate) set.sharedReloadCompleteTick = sharedDeadline;
        for (ObjectWeaponSlotRuntime& slot : set.slots) {
            if (slot.content) slot.nextReadyTick = sharedDeadline;
        }
    }
    return true;
}

void ObjectCombatSystem::executeSystemWeaponFires(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content,
    const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players,
    container::Span<const ObjectSystemWeaponFireCommand> commands,
    uint32_t logicFramesPerSecond,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectProjectileSpawnRequest>* outProjectiles) {
    container::Vector<const ObjectSystemWeaponFireCommand*> ordered;
    ordered.reserve(commands.size());
    for (const ObjectSystemWeaponFireCommand& command : commands) {
        ordered.push_back(&command);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const ObjectSystemWeaponFireCommand* left,
           const ObjectSystemWeaponFireCommand* right) {
            if (left->emissionSequence != right->emissionSequence) {
                return left->emissionSequence < right->emissionSequence;
            }
            if (left->source != right->source) return left->source < right->source;
            if (left->authoredOrder != right->authoredOrder) {
                return left->authoredOrder < right->authoredOrder;
            }
            return left->sourceShotSequence < right->sourceShotSequence;
        });

    for (const ObjectSystemWeaponFireCommand* command : ordered) {
        if (!command || !command->source || !command->content) continue;
        const std::optional<ecs::entity> sourceEntity =
            lifecycle.entityFromIdIncludingPending(command->source);
        const game::WeaponTemplate* definition =
            content.findWeapon(command->content);
        if (!sourceEntity || !registry.valid(*sourceEntity) || !definition) continue;
        const ObjectStatusComponent* sourceStatus =
            ecs::try_get<ObjectStatusComponent>(registry, *sourceEntity);
        if (sourceStatus && sourceStatus->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Sold))) continue;
        const game::WeaponBonus bonus = content.resolveWeaponBonus(
            *definition, command->bonusConditions);
        const auto firingConditions = command->usesFiringPresentation
            ? firingPresentationConditions(
                  registry, *sourceEntity, command->launchSlot)
            : std::nullopt;
        const WeaponLaunchTransform weaponLaunch =
            pristineWeaponLaunchTransform(
                registry, *sourceEntity, content,
                command->launchSlot,
                command->sourceBarrelSequenceOrdinal,
                command->sourcePosition,
                firingConditions ? &*firingConditions : nullptr);

        if (!definition->projectileObject.empty()) {
            // A missing projectile sink must not silently turn a physical
            // projectile into immediate warhead damage.  Admission already
            // advanced the private Weapon runtime, so still publish Fired;
            // normal GameSession integration always supplies this stream.
            if (outProjectiles) {
                ObjectProjectileSpawnRequest projectileRequest{
                    .launcher = command->source,
                    .sourcePathfindLayer = [&]() {
                        const ObjectTerrainLayerComponent* layer =
                            ecs::try_get<ObjectTerrainLayerComponent>(
                                registry, *sourceEntity);
                        return layer ? layer->pathfindLayer
                                     : game::terrain::kGroundPathfindLayer;
                    }(),
                    .intendedTarget = command->target,
                    .detonationWeapon = command->content,
                    .launcherWeaponBonusConditions = command->bonusConditions,
                    .projectileTemplate = definition->projectileObject,
                    .launchPosition = weaponLaunch.position,
                    .projectileStreamOwnerAnchorPosition =
                        command->sourcePosition,
                    .launchOrientation = weaponLaunch.orientation,
                    .targetPosition = command->impactPosition,
                    .scatteredTargetPathfindLayer =
                        command->scatteredTargetPathfindLayer,
                    .intendedTargetBasePosition =
                        command->intendedTargetBasePosition,
                    .launcherVelocityUnitsPerSecond = [&]() {
                        const ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(registry, *sourceEntity);
                        return physics ? physics->velocityUnitsPerSecond
                                       : LogicFixedVec3{};
                    }(),
                    .projectileExhaust = [&]() {
                        size_t level = 0;
                        if (const ObjectVeterancyComponent* veterancy =
                                ecs::try_get<ObjectVeterancyComponent>(
                                    registry, *sourceEntity)) {
                            level = std::min<size_t>(
                                static_cast<size_t>(veterancy->level),
                                game::WeaponTemplate::kVeterancyLevelCount - 1);
                        }
                        return definition->projectileExhausts[level];
                    }(),
                    .projectileStreamOwnerGeneration =
                        command->projectileStreamOwnerGeneration,
                    .launchSlot = static_cast<uint8_t>(command->launchSlot),
                    .sourceShotSequence = command->sourceShotSequence,
                    .sourceBarrelSequenceOrdinal =
                        command->sourceBarrelSequenceOrdinal,
                    .waypointPathStartId = command->waypointPathStartId,
                    .waypointGraphRevision = command->waypointGraphRevision,
                    .hasLaunchOrientation = weaponLaunch.hasOrientation,
                    .hasIntendedTargetBasePosition =
                        command->hasIntendedTargetBasePosition,
                    .targetWasScattered = command->targetWasScattered,
                    .hasTumbleAngularRates = command->hasTumbleAngularRates,
                    .tumbleYawRate = command->tumbleYawRate,
                    .tumblePitchRate = command->tumblePitchRate,
                    .tumbleRollRate = command->tumbleRollRate,
                    .confirmedTick = command->confirmedTick,
                };
                outProjectiles->push_back(std::move(projectileRequest));
            }
        } else {
            const LogicFixedVec3 impactPosition =
                definition->damageDealtAtSelfPosition
                    ? command->sourcePosition : command->impactPosition;
            processHistoricWeaponImpact(
                registry, *definition, command->content, command->source,
                impactPosition, command->sourceShotSequence,
                logicFramesPerSecond, command->confirmedTick,
                m_historicBonusWeaponFires);
            appendWeaponImpactDamage(registry, lifecycle, spatialIndex, players, {
                .filterSource = command->source,
                .damageCredit = command->source,
                .producer = command->source,
                .filterSourceEntity = *sourceEntity,
                .primaryTarget = definition->damageDealtAtSelfPosition
                    ? INVALID_OBJECT_ID : command->target,
                .impactPosition = impactPosition,
                .weapon = definition,
                .bonus = bonus,
                .sourceSequence = command->sourceShotSequence,
                .confirmedTick = command->confirmedTick,
            }, outDamage, m_weaponDamageVictimScratch);
        }
        m_events.push_back({
            .kind = ObjectWeaponEventKind::Fired,
            .source = command->source,
            .sourcePlayer = [&]() {
                const OwnerComponent* owner =
                    ecs::try_get<OwnerComponent>(registry, *sourceEntity);
                return owner ? owner->player : INVALID_PLAYER_ID;
            }(),
            .target = command->target,
            .sourceShotSequence = command->sourceShotSequence,
            .sourceBarrelSequenceOrdinal =
                command->sourceBarrelSequenceOrdinal,
            .slot = command->launchSlot,
            .content = command->content,
            .veterancy = [&]() {
                const ObjectVeterancyComponent* veterancy =
                    ecs::try_get<ObjectVeterancyComponent>(
                        registry, *sourceEntity);
                return veterancy
                    ? veterancy->level
                    : game::ObjectVeterancyLevel::Regular;
            }(),
            .fxPolicy = resolveObjectWeaponFxPolicy(
                registry, *sourceEntity, &lifecycle, players, *definition,
                command->weaponFxSuspendedByDelay),
            .weaponName = definition->name,
            .sourcePosition = command->sourcePosition,
            .impactPosition = command->impactPosition,
            .hasFrozenPositions = true,
            .usesFiringPresentation = command->usesFiringPresentation,
            .recoilDirectionRadians = math::fixed_atan2(
                command->impactPosition.y - command->sourcePosition.y,
                command->impactPosition.x - command->sourcePosition.x),
            .confirmedTick = command->confirmedTick,
        });
    }
}

} // namespace engine
