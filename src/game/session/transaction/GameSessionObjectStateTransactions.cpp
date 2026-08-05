#include "game/session/transaction/GameSessionObjectStateTransactions.h"

#include "game/base/GameBalanceConstants.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/script/runtime/ScriptOrderActions.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

[[nodiscard]] container::String canonicalDrawableCaption(
    container::StringView source) {
    container::String result;
    result.reserve(std::min(
        source.size(), ObjectDrawableCaptionComponent::MaximumCodePoints * 4u));
    size_t offset = 0;
    size_t codePoints = 0;
    while (offset < source.size() &&
           codePoints < ObjectDrawableCaptionComponent::MaximumCodePoints) {
        const unsigned char lead = static_cast<unsigned char>(source[offset]);
        if (lead == 0u) break;
        size_t bytes = 1;
        if ((lead & 0xe0u) == 0xc0u) bytes = 2;
        else if ((lead & 0xf0u) == 0xe0u) bytes = 3;
        else if ((lead & 0xf8u) == 0xf0u) bytes = 4;
        if (offset + bytes > source.size()) bytes = 1;
        for (size_t index = 1; index < bytes; ++index) {
            if ((static_cast<unsigned char>(source[offset + index]) & 0xc0u)
                    != 0x80u) {
                bytes = 1;
                break;
            }
        }
        if (bytes == 1 && lead < 0x20u) result.push_back(' ');
        else result.append(source.substr(offset, bytes));
        offset += bytes;
        ++codePoints;
    }
    return result;
}

} // namespace

GameSessionObjectStateTransactions::GameSessionObjectStateTransactions(
    ecs::registry& registry, ObjectLifecycle& objects) noexcept
    : m_registry(registry), m_objects(objects) {}

ecs::entity GameSessionObjectStateTransactions::liveEntity(
    ObjectId object) const noexcept {
    if (!object || m_objects.isPendingDestroy(object)) return ecs::null;
    return m_objects.entityFromId(object).value_or(ecs::null);
}

bool GameSessionObjectStateTransactions::setDrawableCaption(
    ObjectId object, container::StringView text, uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    const container::String canonical = canonicalDrawableCaption(text);
    ObjectDrawableCaptionComponent* caption =
        ecs::try_get<ObjectDrawableCaptionComponent>(m_registry, entity);
    if (canonical.empty()) {
        if (!caption) return false;
        ecs::remove<ObjectDrawableCaptionComponent>(m_registry, entity);
        markObjectDirty(
            m_registry, entity, ObjectDirtyDomain::RenderExtraction);
        return true;
    }
    if (!caption) {
        ecs::emplace<ObjectDrawableCaptionComponent>(
            m_registry, entity,
            ObjectDrawableCaptionComponent{
                .text = canonical,
                .lastChangedTick = confirmedTick,
                .revision = 1,
            });
        markObjectDirty(
            m_registry, entity, ObjectDirtyDomain::RenderExtraction);
        return true;
    }
    if (caption->text == canonical) return false;
    caption->text = canonical;
    caption->lastChangedTick = confirmedTick;
    if (caption->revision != std::numeric_limits<uint64_t>::max()) {
        ++caption->revision;
    }
    markObjectDirty(
        m_registry, entity, ObjectDirtyDomain::RenderExtraction);
    return true;
}

bool GameSessionObjectStateTransactions::markSingleUseCommandUsed(
    ObjectId object, uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity =
        m_objects.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(m_registry, *entity);
    if (!status) return false;
    if (status->singleUseCommandUsed) return true;
    status->singleUseCommandUsed = true;
    if (status->revision != std::numeric_limits<uint64_t>::max()) {
        ++status->revision;
    }
    status->lastChangedTick = confirmedTick;
    return true;
}

bool GameSessionObjectStateTransactions::setHeld(
    ObjectId object, bool held, uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    const ObjectDisabledTransition transition = held
        ? ObjectDisabledSystem::setUntil(
              m_registry, entity, ObjectDisabledReason::Held,
              OBJECT_DISABLED_FOREVER_TICK, confirmedTick)
        : ObjectDisabledSystem::clear(
              m_registry, entity, ObjectDisabledReason::Held, confirmedTick);
    return transition.changed();
}

bool GameSessionObjectStateTransactions::setUnmanned(
    ObjectId object, uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    return ObjectDisabledSystem::setUntil(
        m_registry, entity, ObjectDisabledReason::Unmanned,
        OBJECT_DISABLED_FOREVER_TICK, confirmedTick).changed();
}

bool GameSessionObjectStateTransactions::setRepulsor(
    ObjectId object, bool repulsor, uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    const game::ObjectStatusMask mask =
        game::objectStatusBit(game::ObjectStatusFlag::Repulsor);
    return ObjectStatusSystem::apply(
        m_registry, entity,
        {.setMask = repulsor ? mask : 0,
         .clearMask = repulsor ? 0 : mask,
         .confirmedTick = confirmedTick}).changed();
}

bool GameSessionObjectStateTransactions::setStealthEnabled(
    ObjectId object, bool enabled, uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    const game::ObjectStatusMask mask =
        game::objectStatusBit(game::ObjectStatusFlag::ScriptUnstealthed);
    return ObjectStatusSystem::apply(
        m_registry, entity,
        {.setMask = enabled ? 0 : mask,
         .clearMask = enabled ? mask : 0,
         .confirmedTick = confirmedTick}).changed();
}

bool GameSessionObjectStateTransactions::setPanelFlag(
    ObjectId object, ObjectPanelFlag flag, bool enabled,
    uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;

    switch (flag) {
    case ObjectPanelFlag::Enabled: {
        const ObjectDisabledTransition transition = enabled
            ? ObjectDisabledSystem::clear(
                  m_registry, entity, ObjectDisabledReason::ScriptDisabled,
                  confirmedTick)
            : ObjectDisabledSystem::setUntil(
                  m_registry, entity, ObjectDisabledReason::ScriptDisabled,
                  OBJECT_DISABLED_FOREVER_TICK, confirmedTick);
        return transition.changed();
    }
    case ObjectPanelFlag::Powered: {
        const ObjectDisabledTransition transition = enabled
            ? ObjectDisabledSystem::clear(
                  m_registry, entity,
                  ObjectDisabledReason::ScriptUnderpowered, confirmedTick)
            : ObjectDisabledSystem::setUntil(
                  m_registry, entity,
                  ObjectDisabledReason::ScriptUnderpowered,
                  OBJECT_DISABLED_FOREVER_TICK, confirmedTick);
        return transition.changed();
    }
    case ObjectPanelFlag::Indestructible: {
        ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_registry, entity);
        if (!health || !health->acceptsDamage) return false;
        bool changed = health->indestructible != enabled;
        health->indestructible = enabled;
        if (ecs::try_get<ObjectBridgeComponent>(m_registry, entity)) {
            const auto towers = ecs::view<
                const ObjectBridgeTowerComponent, ObjectHealthComponent>(
                    m_registry);
            for (const ecs::entity towerEntity : towers) {
                const ObjectBridgeTowerComponent& tower =
                    towers.template get<const ObjectBridgeTowerComponent>(
                        towerEntity);
                if (tower.bridge != object) continue;
                ObjectHealthComponent& towerHealth =
                    towers.template get<ObjectHealthComponent>(towerEntity);
                if (towerHealth.indestructible == enabled) continue;
                towerHealth.indestructible = enabled;
                changed = true;
            }
        }
        return changed;
    }
    case ObjectPanelFlag::Selectable: {
        const game::ObjectStatusMask mask =
            game::objectStatusBit(game::ObjectStatusFlag::Unselectable);
        return ObjectStatusSystem::apply(
            m_registry, entity,
            {.setMask = enabled ? 0 : mask,
             .clearMask = enabled ? mask : 0,
             .confirmedTick = confirmedTick}).changed();
    }
    case ObjectPanelFlag::AiRecruitable: {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_registry, entity);
        if (!type || !type->archetype || !type->archetype->hasAiUpdate)
            return false;
        ObjectScriptPanelPolicyComponent* policy =
            ecs::try_get<ObjectScriptPanelPolicyComponent>(m_registry, entity);
        if (!policy && enabled) return false;
        if (!policy) {
            policy = &ecs::emplace<ObjectScriptPanelPolicyComponent>(
                m_registry, entity);
        }
        if (policy->aiRecruitable == enabled) return false;
        policy->aiRecruitable = enabled;
        ++policy->revision;
        return true;
    }
    case ObjectPanelFlag::Unsellable:
    case ObjectPanelFlag::PlayerTargetable: {
        ObjectScriptPanelPolicyComponent* policy =
            ecs::try_get<ObjectScriptPanelPolicyComponent>(m_registry, entity);
        if (!policy && !enabled) return false;
        if (!policy) {
            policy = &ecs::emplace<ObjectScriptPanelPolicyComponent>(
                m_registry, entity);
        }
        bool& value = flag == ObjectPanelFlag::Unsellable
            ? policy->unsellable
            : policy->playerTargetable;
        if (value == enabled) return false;
        value = enabled;
        ++policy->revision;
        return true;
    }
    }
    return false;
}

bool GameSessionObjectStateTransactions::setCaveIndex(
    ObjectId object, int32_t caveIndex) {
    if (caveIndex < 0) return false;
    const ecs::entity entity = liveEntity(object);
    ObjectContainmentRuntimeComponent* cave = entity != ecs::null
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(m_registry, entity)
        : nullptr;
    if (!cave || !cave->hasCave || cave->caveIndex == caveIndex) return false;

    const int32_t oldIndex = cave->caveIndex;
    const auto view = ecs::view<
        const ObjectIdentityComponent,
        const ObjectContainmentRuntimeComponent,
        const ObjectContainmentComponent>(m_registry);
    for (const ecs::entity candidate : view) {
        const ObjectContainmentRuntimeComponent& runtime =
            view.template get<const ObjectContainmentRuntimeComponent>(
                candidate);
        if (!runtime.hasCave || !runtime.plan ||
            (runtime.caveIndex != oldIndex &&
             runtime.caveIndex != caveIndex)) {
            continue;
        }
        const ObjectContainmentComponent& contents =
            view.template get<const ObjectContainmentComponent>(candidate);
        const ObjectId entrance =
            view.template get<const ObjectIdentityComponent>(candidate).id;
        for (const ObjectContainedObjectRecord& record : contents.objects) {
            const std::optional<ecs::entity> passenger =
                m_objects.entityFromIdIncludingPending(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(m_registry,
                                                            *passenger)
                : nullptr;
            if (edge && edge->container == entrance &&
                edge->containmentRuleIndex < runtime.plan->rules.size() &&
                runtime.plan->rules[edge->containmentRuleIndex].kind ==
                    ObjectContainmentKind::Cave) {
                return false;
            }
        }
    }
    cave->caveIndex = caveIndex;
    ++cave->caveIndexRevision;
    return true;
}

bool GameSessionObjectStateTransactions::setRailroadHeld(
    ObjectId object, bool held) {
    const ecs::entity entity = liveEntity(object);
    ObjectRailroadComponent* railroad = entity != ecs::null
        ? ecs::try_get<ObjectRailroadComponent>(m_registry, entity)
        : nullptr;
    if (!railroad || railroad->instances.empty() ||
        railroad->instances.front().held == held) {
        return false;
    }
    railroad->instances.front().held = held;
    ++railroad->instances.front().revision;
    return true;
}

bool GameSessionObjectStateTransactions::setStoppingDistance(
    ObjectId object, math::q32_32 distance) {
    const ecs::entity entity = liveEntity(object);
    ObjectLocomotionComponent* locomotion = entity != ecs::null
        ? ecs::try_get<ObjectLocomotionComponent>(m_registry, entity)
        : nullptr;
    if (!locomotion) return false;
    if (distance >= math::q32_32::from_fraction(1, 2) &&
        locomotion->closeEnough != distance) {
        locomotion->closeEnough = distance;
    }
    return true;
}

bool GameSessionObjectStateTransactions::setSupplyTruckIdleSuppressed(
    ObjectId object, bool suppressed, uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    ObjectEconomyComponent* economy = entity != ecs::null
        ? ecs::try_get<ObjectEconomyComponent>(m_registry, entity)
        : nullptr;
    if (!economy || economy->supplyTrucks.empty()) return false;
    bool changed = false;
    for (ObjectSupplyTruckRuntime& runtime : economy->supplyTrucks) {
        // Only the suppressed -> unsuppressed edge restarts the warehouse
        // cycle. A script that re-asserts the value it already holds must not
        // discard in-flight docking progress, or the truck never completes a
        // cycle while that script keeps running.
        if (suppressed) {
            if (runtime.scriptIdleSuppressed) continue;
            runtime.scriptIdleSuppressed = true;
            changed = true;
        } else {
            if (!runtime.scriptIdleSuppressed &&
                !runtime.externalIdleSuppressed &&
                runtime.workerSupplyActive) {
                continue;
            }
            runtime.scriptIdleSuppressed = false;
            runtime.externalIdleSuppressed = false;
            runtime.workerSupplyActive = true;
            runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
            runtime.targetDock = INVALID_OBJECT_ID;
            runtime.targetDockModule = 0;
            runtime.targetIsCenter = false;
            runtime.approachPosition = -1;
            runtime.nextActionTick = confirmedTick;
            changed = true;
        }
    }
    return changed;
}

bool GameSessionObjectStateTransactions::assignSupplyTruckPreferredDock(
    ObjectId object, ObjectId center, uint64_t confirmedTick) {
    const ecs::entity truckEntity = liveEntity(object);
    const ecs::entity centerEntity = liveEntity(center);
    ObjectEconomyComponent* truck = truckEntity != ecs::null
        ? ecs::try_get<ObjectEconomyComponent>(m_registry, truckEntity)
        : nullptr;
    const ObjectEconomyComponent* supplyCenter = centerEntity != ecs::null
        ? ecs::try_get<ObjectEconomyComponent>(m_registry, centerEntity)
        : nullptr;
    const OwnerComponent* truckOwner = truckEntity != ecs::null
        ? ecs::try_get<OwnerComponent>(m_registry, truckEntity) : nullptr;
    const OwnerComponent* centerOwner = centerEntity != ecs::null
        ? ecs::try_get<OwnerComponent>(m_registry, centerEntity) : nullptr;
    if (!truck || truck->supplyTrucks.empty() || !supplyCenter ||
        supplyCenter->supplyCenterDocks.empty() || !truckOwner ||
        !centerOwner || !truckOwner->player ||
        truckOwner->player != centerOwner->player) {
        return false;
    }
    bool changed = false;
    for (ObjectSupplyTruckRuntime& runtime : truck->supplyTrucks) {
        if (runtime.scriptIdleSuppressed) continue;
        changed = changed || runtime.preferredDock != center ||
            runtime.externalIdleSuppressed ||
            !runtime.workerSupplyActive;
        runtime.preferredDock = center;
        runtime.externalIdleSuppressed = false;
        runtime.workerSupplyActive = true;
        // Do not tear down an in-flight supply cycle here. Dock reservations
        // are owned and released by ObjectEconomySystem; clearing only the
        // truck-side target would strand a live approach owner forever. The
        // preferred center is observed naturally when this cycle chooses its
        // delivery dock, while an idle truck observes it on its next update.
        if (runtime.state == ObjectSupplyTruckRuntimeState::SeekingWarehouse)
            runtime.nextActionTick = confirmedTick;
    }
    return changed;
}

bool GameSessionObjectStateTransactions::setWarehouseCashValue(
    ObjectId object, int32_t cashValue) {
    const ecs::entity entity = liveEntity(object);
    ObjectEconomyComponent* economy = entity != ecs::null
        ? ecs::try_get<ObjectEconomyComponent>(m_registry, entity)
        : nullptr;
    if (!economy || economy->supplyWarehouseDocks.empty()) return false;

    const int64_t normalizedCash = std::max<int64_t>(0, cashValue);
    const int64_t baseValue =
        std::max<int64_t>(1, DEFAULT_BASE_VALUE_PER_SUPPLY_BOX);
    const uint64_t boxes = static_cast<uint64_t>(
        (normalizedCash + baseValue - 1) / baseValue);
    ObjectSupplyWarehouseDockRuntime& warehouse =
        economy->supplyWarehouseDocks.front();
    const uint32_t normalizedBoxes = static_cast<uint32_t>(
        std::min<uint64_t>(boxes, std::numeric_limits<uint32_t>::max()));
    if (warehouse.boxesStored == normalizedBoxes) return false;
    warehouse.boxesStored = normalizedBoxes;
    ++warehouse.revision;
    return true;
}

bool GameSessionObjectStateTransactions::mutateSpecialPowerCountdown(
    ObjectId object, const SpecialPowerDefinition& definition,
    script::ScriptSpecialPowerCountdownOperation operation,
    int32_t seconds, bool paused, int32_t logicFramesPerSecond,
    uint64_t confirmedTick) {
    const ecs::entity entity = liveEntity(object);
    ObjectSpecialPowerComponent* source = entity != ecs::null
        ? ecs::try_get<ObjectSpecialPowerComponent>(m_registry, entity)
        : nullptr;
    if (!source) return false;

    ObjectSpecialPowerRuntime* sourceRuntime = nullptr;
    for (ObjectSpecialPowerRuntime& runtime : source->instances) {
        if (runtime.content == definition.id) {
            sourceRuntime = &runtime;
            break;
        }
    }
    if (!sourceRuntime) return false;

    if (operation == script::ScriptSpecialPowerCountdownOperation::Pause) {
        if (paused) {
            if (sourceRuntime->pausedCount == 0)
                sourceRuntime->pauseStartedTick = confirmedTick;
            if (sourceRuntime->pausedCount !=
                std::numeric_limits<uint32_t>::max()) {
                ++sourceRuntime->pausedCount;
            }
            return true;
        }
        if (sourceRuntime->pausedCount == 0) return false;
        --sourceRuntime->pausedCount;
        if (sourceRuntime->pausedCount == 0) {
            const uint64_t pausedTicks =
                confirmedTick >= sourceRuntime->pauseStartedTick
                ? confirmedTick - sourceRuntime->pauseStartedTick
                : 0;
            if (!definition.sharedSyncedTimer) {
                sourceRuntime->readyTick = pausedTicks >
                        std::numeric_limits<uint64_t>::max() -
                            sourceRuntime->readyTick
                    ? std::numeric_limits<uint64_t>::max()
                    : sourceRuntime->readyTick + pausedTicks;
            }
            sourceRuntime->pauseStartedTick = 0;
        }
        return true;
    }

    const int64_t framesPerSecond = std::max(1, logicFramesPerSecond);
    const int64_t delta = static_cast<int64_t>(seconds) * framesPerSecond;
    const auto applyDelta = [operation, delta, confirmedTick, &definition](
                                ObjectSpecialPowerRuntime& runtime) {
        const uint64_t base =
            operation == script::ScriptSpecialPowerCountdownOperation::Set
            ? (runtime.pausedCount != 0 && !definition.sharedSyncedTimer
                   ? runtime.pauseStartedTick
                   : confirmedTick)
            : runtime.readyTick;
        if (delta >= 0) {
            const uint64_t amount = static_cast<uint64_t>(delta);
            runtime.readyTick =
                amount > std::numeric_limits<uint64_t>::max() - base
                ? std::numeric_limits<uint64_t>::max()
                : base + amount;
        } else {
            const uint64_t amount =
                static_cast<uint64_t>(-(delta + 1)) + 1u;
            runtime.readyTick = amount > base ? 0 : base - amount;
        }
    };

    if (!definition.sharedSyncedTimer) {
        applyDelta(*sourceRuntime);
        return true;
    }

    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_registry, entity);
    if (!owner || !owner->player) return false;
    bool changed = false;
    const auto view =
        ecs::view<const OwnerComponent, ObjectSpecialPowerComponent>(
            m_registry);
    for (const ecs::entity candidate : view) {
        const OwnerComponent& candidateOwner =
            view.template get<const OwnerComponent>(candidate);
        if (candidateOwner.player != owner->player) continue;
        ObjectSpecialPowerComponent& powers =
            view.template get<ObjectSpecialPowerComponent>(candidate);
        for (ObjectSpecialPowerRuntime& runtime : powers.instances) {
            if (runtime.content != definition.id) continue;
            applyDelta(runtime);
            changed = true;
        }
    }
    return changed;
}

bool GameSessionObjectStateTransactions::setAIAttitude(
    ObjectId object, ObjectAIAttitude attitude) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    ObjectAIBehaviorPolicyComponent* policy =
        ecs::try_get<ObjectAIBehaviorPolicyComponent>(m_registry, entity);
    if (!policy) {
        ecs::emplace<ObjectAIBehaviorPolicyComponent>(
            m_registry, entity,
            ObjectAIBehaviorPolicyComponent{.attitude = attitude});
        return true;
    }
    if (policy->attitude == attitude) return false;
    policy->attitude = attitude;
    ++policy->revision;
    if (policy->revision == 0) ++policy->revision;
    return true;
}

bool GameSessionObjectStateTransactions::setAttackPrioritySetId(
    ObjectId object, uint32_t setId) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    ObjectAIBehaviorPolicyComponent* policy =
        ecs::try_get<ObjectAIBehaviorPolicyComponent>(m_registry, entity);
    if (!policy) {
        ecs::emplace<ObjectAIBehaviorPolicyComponent>(
            m_registry, entity,
            ObjectAIBehaviorPolicyComponent{.attackPrioritySetId = setId});
        return true;
    }
    if (policy->attackPrioritySetId == setId) return false;
    policy->attackPrioritySetId = setId;
    ++policy->revision;
    if (policy->revision == 0) ++policy->revision;
    return true;
}

bool GameSessionObjectStateTransactions::setScriptToppleDirection(
    ObjectId object, LogicFixedVec3 direction) {
    const ecs::entity entity = liveEntity(object);
    if (entity == ecs::null) return false;
    ObjectScriptToppleDirectionComponent* component =
        ecs::try_get<ObjectScriptToppleDirectionComponent>(
            m_registry, entity);
    if (!component) {
        ecs::emplace<ObjectScriptToppleDirectionComponent>(
            m_registry, entity,
            ObjectScriptToppleDirectionComponent{
                .direction = direction,
                .revision = 1,
            });
        return true;
    }
    if (component->direction.x == direction.x &&
        component->direction.y == direction.y &&
        component->direction.z == direction.z) {
        return false;
    }
    component->direction = direction;
    ++component->revision;
    if (component->revision == 0) ++component->revision;
    return true;
}

} // namespace engine
